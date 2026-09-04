#include "analytics/event_engine.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
	int64_t us;
	bool valid;
} MeTimeline;

typedef struct {
	const MeRuleFact *first;
	uint32_t match_count;
	uint32_t new_match_count;
	uint32_t delta_in;
	uint32_t delta_out;
	uint32_t track_count;
	uint64_t track_ids[ME_ANALYTICS_EVENT_MAX_TRACKS];
	uint32_t fact_count;
	const MeRuleFact *facts[ME_ANALYTICS_MAX_RULE_FACTS];
} MeRuleSummary;

static void event_set_err(char *err, size_t errsz, const char *fmt, ...)
{
	va_list ap;

	if (!err || errsz == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

static bool valid_event_reason(MeEventReason reason)
{
	return reason >= ME_EVENT_REASON_CONFIRMED &&
	       reason <= ME_EVENT_REASON_PROCESS_RESTART;
}

static uint64_t channel_hash(const char *channel_id)
{
	const unsigned char *p = (const unsigned char *)channel_id;
	uint64_t hash = 1469598103934665603ULL;

	while (*p) {
		hash ^= *p++;
		hash *= 1099511628211ULL;
	}
	return hash;
}

static bool scale_to_us(uint64_t value, uint64_t numerator,
			       uint64_t denominator, int64_t *out)
{
	__uint128_t scaled;
	__uint128_t result;

	if (!out || denominator == 0)
		return false;
	scaled = (__uint128_t)value * numerator * 1000000U;
	result = scaled / denominator;
	if (result > (__uint128_t)INT64_MAX)
		return false;
	*out = (int64_t)result;
	return true;
}

static bool pts_to_us(const MeNormalizedObservation *observation,
			     int64_t *out)
{
	if (!observation || !out || !observation->source_pts_valid ||
	    observation->source_pts < 0 || observation->source_timebase.num == 0 ||
	    observation->source_timebase.den == 0)
		return false;
	return scale_to_us((uint64_t)observation->source_pts,
				   observation->source_timebase.num,
				   observation->source_timebase.den, out);
}

/* This timeline is only for local durations. Alarm event_time_us remains the
 * captured wall-clock value and is never filled from this fallback. */
static MeTimeline observation_timeline(const MeEventEngine *engine,
					       const MeNormalizedObservation *observation)
{
	MeTimeline timeline = {0, false};

	if (pts_to_us(observation, &timeline.us)) {
		timeline.valid = true;
		return timeline;
	}
	if (observation &&
	    (observation->clock_state == ME_CLOCK_STATE_SYNCED ||
	     observation->clock_state == ME_CLOCK_STATE_ESTIMATED) &&
	    observation->capture_time_us > 0) {
		timeline.us = observation->capture_time_us;
		timeline.valid = true;
		return timeline;
	}
	if (engine && engine->config.fps > 0 && observation &&
	    observation->frame_id > 0) {
		if (scale_to_us(observation->frame_id - 1, 1,
				(uint64_t)engine->config.fps, &timeline.us))
			timeline.valid = true;
	}
	return timeline;
}

static bool elapsed_at_least(MeTimeline now, int64_t then_us,
				     bool then_valid, uint64_t duration_us)
{
	uint64_t elapsed;

	if (!now.valid || !then_valid || now.us < then_us)
		return false;
	elapsed = (uint64_t)(now.us - then_us);
	return elapsed >= duration_us;
}

static bool time_add_ms(int64_t start_us, uint32_t duration_ms,
				int64_t *out)
{
	int64_t duration_us = (int64_t)duration_ms * 1000;

	if (!out || start_us < 0 || duration_us < 0)
		return false;
	if (start_us > INT64_MAX - duration_us)
		*out = INT64_MAX;
	else
		*out = start_us + duration_us;
	return true;
}

static void saturating_add_u32(uint32_t *value, uint32_t addend)
{
	if (UINT32_MAX - *value < addend)
		*value = UINT32_MAX;
	else
		*value += addend;
}

static int emit_event(MeEventEngine *engine, const MeAnalyticsEvent *event)
{
	if (engine->sink)
		engine->sink(engine->sink_userdata, event);
	return 1;
}

static bool increment_event_seq(MeAnalyticsEvent *event)
{
	if (!event || event->event_seq == UINT64_MAX)
		return false;
	event->event_seq++;
	return true;
}

static void event_copy_observation_context(
	MeAnalyticsEvent *event, const MeNormalizedObservation *observation,
	uint32_t person_count, uint32_t delta_in, uint32_t delta_out)
{
	if (!event || !observation)
		return;
	snprintf(event->channel_id, sizeof(event->channel_id), "%s",
		 observation->channel_id);
	event->stream_epoch = observation->stream_epoch;
	event->event_time_us =
		(observation->clock_state == ME_CLOCK_STATE_SYNCED ||
		 observation->clock_state == ME_CLOCK_STATE_ESTIMATED)
			? observation->capture_time_us
			: 0;
	event->clock_state = observation->clock_state;
	event->source_pts = observation->source_pts;
	event->source_pts_valid = observation->source_pts_valid;
	event->source_timebase = observation->source_timebase;
	event->frame_id = observation->frame_id;
	event->person_count = person_count;
	event->delta_in = delta_in;
	event->delta_out = delta_out;
}

static void event_copy_responsible_tracks(MeAnalyticsEvent *event,
						  const uint64_t *track_ids,
						  uint32_t track_count)
{
	uint32_t count;

	if (!event)
		return;
	count = track_count > ME_ANALYTICS_EVENT_MAX_TRACKS
			? ME_ANALYTICS_EVENT_MAX_TRACKS
			: track_count;
	event->responsible_track_count = count;
	if (count > 0 && track_ids)
		memcpy(event->responsible_track_ids, track_ids,
		       (size_t)count * sizeof(track_ids[0]));
	if (count < ME_ANALYTICS_EVENT_MAX_TRACKS)
		memset(&event->responsible_track_ids[count], 0,
		       (size_t)(ME_ANALYTICS_EVENT_MAX_TRACKS - count) *
		       sizeof(event->responsible_track_ids[0]));
}

static void clear_trigger_candidate(MeEventEngine *engine)
{
	engine->trigger_pending = false;
	engine->trigger_frames = 0;
	engine->trigger_since_us = 0;
	engine->trigger_since_valid = false;
}

static void clear_pending(MeEventEngine *engine)
{
	clear_trigger_candidate(engine);
	engine->absent_since_us = 0;
	engine->absent_since_valid = false;
	engine->update_pending = false;
	engine->pending_person_count = 0;
	engine->pending_delta_in = 0;
	engine->pending_delta_out = 0;
	engine->pending_track_count = 0;
	memset(engine->pending_track_ids, 0, sizeof(engine->pending_track_ids));
	engine->pending_reason = ME_EVENT_REASON_NONE;
	engine->last_emit_timeline_us = 0;
	engine->last_emit_timeline_valid = false;
}

static void clear_tracks(MeEventEngine *engine)
{
	memset(engine->tracks, 0, sizeof(engine->tracks));
}

static void clear_rule_fact_states(MeEventEngine *engine)
{
	memset(engine->rule_fact_states, 0, sizeof(engine->rule_fact_states));
}

static int find_rule_fact_state(const MeEventEngine *engine, uint64_t track_id)
{
	uint32_t i;

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		if (engine->rule_fact_states[i].in_use &&
		    engine->rule_fact_states[i].track_id == track_id)
			return (int)i;
	}
	return -1;
}

static int allocate_rule_fact_state(MeEventEngine *engine, uint64_t track_id)
{
	uint32_t i;

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		if (!engine->rule_fact_states[i].in_use) {
			memset(&engine->rule_fact_states[i], 0,
			       sizeof(engine->rule_fact_states[i]));
			engine->rule_fact_states[i].in_use = true;
			engine->rule_fact_states[i].track_id = track_id;
			return (int)i;
		}
	}
	return -1;
}

static bool rule_fact_seen(const MeEventEngine *engine,
				   const MeRuleFact *fact)
{
	int slot;

	if (!engine || !fact || fact->direction > ME_RULE_DIRECTION_OUT)
		return false;
	slot = find_rule_fact_state(engine, fact->track_id);
	return slot >= 0 &&
	       engine->rule_fact_states[slot].direction_present[fact->direction];
}

static bool remember_rule_fact(MeEventEngine *engine, const MeRuleFact *fact)
{
	int slot;

	if (!engine || !fact || fact->direction > ME_RULE_DIRECTION_OUT)
		return false;
	slot = find_rule_fact_state(engine, fact->track_id);
	if (slot < 0)
		slot = allocate_rule_fact_state(engine, fact->track_id);
	if (slot < 0)
		return false;
	engine->rule_fact_states[slot].direction_present[fact->direction] = true;
	return true;
}

static bool remember_rule_facts(MeEventEngine *engine,
					const MeRuleSummary *summary)
{
	uint32_t i;

	/* Replace the previous observation's facts so a later reappearance is a
	 * new directional occurrence instead of a permanently suppressed one. */
	clear_rule_fact_states(engine);
	for (i = 0; i < summary->fact_count; i++) {
		if (!remember_rule_fact(engine, summary->facts[i]))
			return false;
	}
	return true;
}

static void clear_runtime(MeEventEngine *engine)
{
	engine->active = false;
	memset(&engine->active_event, 0, sizeof(engine->active_event));
	engine->active_person_count = 0;
	engine->latest_person_count = 0;
	engine->latest_person_count_valid = false;
	engine->cooldown_until_us = 0;
	engine->cooldown_until_valid = false;
	clear_pending(engine);
	clear_tracks(engine);
	clear_rule_fact_states(engine);
}

static uint32_t latest_person_count(const MeEventEngine *engine)
{
	return engine->latest_person_count_valid ? engine->latest_person_count
							 : engine->active_person_count;
}

static int find_track(const MeEventEngine *engine, uint64_t track_id)
{
	uint32_t i;

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		if (engine->tracks[i].in_use && engine->tracks[i].track_id == track_id)
			return (int)i;
	}
	return -1;
}

static int allocate_track(MeEventEngine *engine, uint64_t track_id)
{
	uint32_t i;

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		if (!engine->tracks[i].in_use) {
			memset(&engine->tracks[i], 0, sizeof(engine->tracks[i]));
			engine->tracks[i].in_use = true;
			engine->tracks[i].track_id = track_id;
			return (int)i;
		}
	}
	return -1;
}

static bool track_inside(const MeEventEngine *engine,
				 const MeNormalizedBBox *bbox)
{
	uint32_t center_x;
	uint32_t center_y;

	if (!engine->config.roi_enabled)
		return true;
	center_x = (bbox->left + bbox->right) / 2;
	center_y = (bbox->top + bbox->bottom) / 2;
	return center_x >= engine->config.roi.left &&
	       center_x <= engine->config.roi.right &&
	       center_y >= engine->config.roi.top &&
	       center_y <= engine->config.roi.bottom;
}

static void clear_inside_candidate(MeEventTrackState *track)
{
	track->inside_candidate = false;
	track->inside_candidate_valid = false;
	track->inside_candidate_since_us = 0;
}

static void update_track_inside(MeEventEngine *engine,
					MeEventTrackState *track, bool raw_inside,
					MeTimeline now)
{
	uint64_t debounce_us = (uint64_t)engine->config.debounce_ms * 1000ULL;

	if (!track->inside_initialized) {
		track->inside = raw_inside;
		track->inside_initialized = true;
		clear_inside_candidate(track);
		return;
	}
	if (raw_inside == track->inside) {
		clear_inside_candidate(track);
		return;
	}
	if (debounce_us == 0 || !now.valid) {
		track->inside = raw_inside;
		clear_inside_candidate(track);
		return;
	}
	if (!track->inside_candidate_valid ||
	    track->inside_candidate != raw_inside) {
		track->inside_candidate = raw_inside;
		track->inside_candidate_valid = true;
		track->inside_candidate_since_us = now.us;
		return;
	}
	if (elapsed_at_least(now, track->inside_candidate_since_us, true,
				    debounce_us)) {
		track->inside = raw_inside;
		clear_inside_candidate(track);
	}
}

static bool track_confirmed(const MeEventEngine *engine,
				    const MeEventTrackState *track,
				    MeTimeline now)
{
	uint64_t elapsed_us =
		(uint64_t)engine->config.confirm_ms * 1000ULL;

	return (engine->config.confirm_frames > 0 &&
		track->seen_frames >= engine->config.confirm_frames) ||
	       elapsed_at_least(now, track->first_timeline_us,
				 track->first_timeline_valid, elapsed_us);
}

static uint32_t update_tracks(MeEventEngine *engine,
				       const MeNormalizedObservation *observation,
				       MeTimeline now)
{
	uint32_t i;
	uint32_t count = 0;

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++)
		engine->tracks[i].present = false;

	for (i = 0; i < observation->track_count; i++) {
		const MeTrackObservation *input = &observation->tracks[i];
		MeEventTrackState *track;
		int slot;

		if (input->class_id != ME_TRACK_CLASS_PERSON)
			continue;
		if (input->state == ME_TRACK_STATE_REMOVED) {
			slot = find_track(engine, input->track_id);
			if (slot >= 0)
				memset(&engine->tracks[slot], 0,
				       sizeof(engine->tracks[slot]));
			continue;
		}
		if ((input->state != ME_TRACK_STATE_NEW &&
		     input->state != ME_TRACK_STATE_ACTIVE) ||
		    input->score_q < engine->config.score_threshold_q)
			continue;

		slot = find_track(engine, input->track_id);
		if (slot < 0)
			slot = allocate_track(engine, input->track_id);
		if (slot < 0)
			continue;
		track = &engine->tracks[slot];
		track->present = true;
		update_track_inside(engine, track,
				    track_inside(engine, &input->bbox), now);
		if (!track->confirmed) {
			if (track->seen_frames < UINT32_MAX)
				track->seen_frames++;
			if (track->seen_frames == 1) {
				track->first_frame_id = observation->frame_id;
				track->first_timeline_us = now.us;
				track->first_timeline_valid = now.valid;
			}
			if (track_confirmed(engine, track, now))
				track->confirmed = true;
		}
		track->last_seen_frame_id = observation->frame_id;
		track->last_timeline_us = now.us;
		track->last_timeline_valid = now.valid;
	}

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		MeEventTrackState *track = &engine->tracks[i];
		uint64_t grace_us =
			(uint64_t)engine->config.disappear_grace_ms * 1000ULL;

		if (!track->in_use)
			continue;
		if (!track->confirmed && !track->present) {
			memset(track, 0, sizeof(*track));
			continue;
		}
		if (!track->present &&
		    (grace_us == 0 ||
		     elapsed_at_least(now, track->last_timeline_us,
				     track->last_timeline_valid, grace_us)))
			memset(track, 0, sizeof(*track));
	}

	for (i = 0; i < ME_ANALYTICS_MAX_TRACKS; i++) {
		const MeEventTrackState *track = &engine->tracks[i];

		if (track->in_use && track->present && track->confirmed && track->inside)
			count++;
	}
	return count;
}

static bool summary_has_track(const MeRuleSummary *summary, uint64_t track_id)
{
	uint32_t i;

	if (track_id == 0)
		return true;
	for (i = 0; i < summary->track_count; i++) {
		if (summary->track_ids[i] == track_id)
			return true;
	}
	return false;
}

static void summary_add_track(MeRuleSummary *summary, uint64_t track_id)
{
	if (track_id == 0 || summary_has_track(summary, track_id) ||
	    summary->track_count == ME_ANALYTICS_EVENT_MAX_TRACKS)
		return;
	summary->track_ids[summary->track_count++] = track_id;
}

static bool summary_has_fact(const MeRuleSummary *summary,
				     const MeRuleFact *fact)
{
	uint32_t i;

	for (i = 0; i < summary->fact_count; i++) {
		const MeRuleFact *previous = summary->facts[i];

		if (previous->rule_type == fact->rule_type &&
		    previous->direction == fact->direction &&
		    previous->track_id == fact->track_id &&
		    !strcmp(previous->rule_id, fact->rule_id))
			return true;
	}
	return false;
}

static void matching_rules(const MeEventEngine *engine,
				   const MeNormalizedObservation *observation,
				   MeRuleSummary *summary)
{
	uint32_t i;

	memset(summary, 0, sizeof(*summary));
	if (engine->config.rule_type == ME_RULE_TYPE_OCCUPANCY)
		return;
	for (i = 0; i < observation->rule_count; i++) {
		const MeRuleFact *rule = &observation->rules[i];

		if (rule->rule_type == engine->config.rule_type &&
		    !strcmp(rule->rule_id, engine->config.rule_id) &&
		    rule->score_q >= engine->config.score_threshold_q) {
			if (summary_has_fact(summary, rule))
				continue;
			if (summary->fact_count < ME_ANALYTICS_MAX_RULE_FACTS)
				summary->facts[summary->fact_count++] = rule;
			if (!summary->first)
				summary->first = rule;
			if (summary->match_count < UINT32_MAX)
				summary->match_count++;
			if (!rule_fact_seen(engine, rule)) {
				if (summary->new_match_count < UINT32_MAX)
					summary->new_match_count++;
				if (rule->direction == ME_RULE_DIRECTION_IN)
					saturating_add_u32(&summary->delta_in, 1);
				else if (rule->direction == ME_RULE_DIRECTION_OUT)
					saturating_add_u32(&summary->delta_out, 1);
				summary_add_track(summary, rule->track_id);
			}
		}
	}
}

static bool cooldown_elapsed(const MeEventEngine *engine, MeTimeline now)
{
	if (!engine->cooldown_until_valid)
		return true;
	if (!now.valid)
		return false;
	return now.us >= engine->cooldown_until_us;
}

static bool new_event_id(MeEventEngine *engine, const char *channel_id,
				 uint64_t stream_epoch, char *out, size_t outsz)
{
	uint64_t serial;
	int written;

	if (!engine || !channel_id || !out || outsz == 0 ||
	    engine->next_event_serial == UINT64_MAX)
		return false;
	serial = engine->next_event_serial + 1;
	written = snprintf(out, outsz, "me-%016llx-%016llx-%016llx",
			   (unsigned long long)channel_hash(channel_id),
			   (unsigned long long)stream_epoch,
			   (unsigned long long)serial);
	if (written < 0 || (size_t)written >= outsz)
		return false;
	engine->next_event_serial = serial;
	return true;
}

static int emit_start(MeEventEngine *engine,
			      const MeNormalizedObservation *observation,
			      const MeRuleSummary *summary, uint32_t person_count,
			      MeTimeline now, char *err, size_t errsz)
{
	MeAnalyticsEvent *event = &engine->active_event;

	memset(event, 0, sizeof(*event));
	event->contract_version = ME_ANALYTICS_EVENT_CONTRACT_VERSION;
	if (!new_event_id(engine, observation->channel_id, observation->stream_epoch,
			  event->event_id, sizeof(event->event_id))) {
		event_set_err(err, errsz, "analytics event id serial is exhausted");
		return -1;
	}
	event->event_type = engine->config.rule_type;
	snprintf(event->rule_id, sizeof(event->rule_id), "%s",
		 engine->config.rule_id);
	event->phase = ME_EVENT_PHASE_START;
	event->event_seq = 1;
	event->reason = ME_EVENT_REASON_CONFIRMED;
	event->config_version = engine->config_version;
	if (summary && summary->first)
		snprintf(event->rule_id, sizeof(event->rule_id), "%s",
			 summary->first->rule_id);
	event_copy_observation_context(event, observation, person_count,
				       summary ? summary->delta_in : 0,
				       summary ? summary->delta_out : 0);
	if (summary)
		event_copy_responsible_tracks(event, summary->track_ids,
					       summary->track_count);
	if (summary && !remember_rule_facts(engine, summary)) {
		event_set_err(err, errsz, "analytics rule fact state is exhausted");
		return -1;
	}
	engine->active = true;
	engine->active_person_count = person_count;
	engine->last_emit_timeline_us = now.us;
	engine->last_emit_timeline_valid = now.valid;
	return emit_event(engine, event);
}

static void mark_update(MeEventEngine *engine, uint32_t person_count,
				uint32_t delta_in, uint32_t delta_out,
				const uint64_t *track_ids, uint32_t track_count,
				MeEventReason reason)
{
	uint32_t i;

	if (!track_ids)
		track_count = 0;
	engine->update_pending = true;
	engine->pending_person_count = person_count;
	if (delta_in > 0)
		saturating_add_u32(&engine->pending_delta_in, delta_in);
	if (delta_out > 0)
		saturating_add_u32(&engine->pending_delta_out, delta_out);
	for (i = 0; i < track_count; i++) {
		uint32_t j;
		bool present = false;

		for (j = 0; j < engine->pending_track_count; j++) {
			if (engine->pending_track_ids[j] == track_ids[i]) {
				present = true;
				break;
			}
		}
		if (!present &&
		    engine->pending_track_count < ME_ANALYTICS_EVENT_MAX_TRACKS)
			engine->pending_track_ids[engine->pending_track_count++] =
				track_ids[i];
	}
	engine->pending_reason = reason;
}

static int maybe_emit_update(MeEventEngine *engine,
				     const MeNormalizedObservation *observation,
				     uint32_t person_count, MeTimeline now,
				     char *err, size_t errsz)
{
	MeAnalyticsEvent *event = &engine->active_event;

	if (!engine->update_pending)
		return 0;
	if (engine->last_emit_timeline_valid && now.valid &&
	    !elapsed_at_least(now, engine->last_emit_timeline_us, true,
			       (uint64_t)engine->config.update_interval_ms * 1000ULL))
		return 0;
	if (!increment_event_seq(event)) {
		event_set_err(err, errsz, "analytics event sequence is exhausted");
		return -1;
	}
	event->phase = ME_EVENT_PHASE_UPDATE;
	event->reason = engine->pending_reason;
	event_copy_observation_context(event, observation, person_count,
				       engine->pending_delta_in,
				       engine->pending_delta_out);
	event_copy_responsible_tracks(event, engine->pending_track_ids,
					       engine->pending_track_count);
	engine->active_person_count = person_count;
	engine->last_emit_timeline_us = now.us;
	engine->last_emit_timeline_valid = now.valid;
	engine->update_pending = false;
	engine->pending_person_count = 0;
	engine->pending_delta_in = 0;
	engine->pending_delta_out = 0;
	engine->pending_reason = ME_EVENT_REASON_NONE;
	return emit_event(engine, event);
}

static int emit_end(MeEventEngine *engine,
			    const MeNormalizedObservation *observation,
			    MeEventReason reason, uint32_t person_count, MeTimeline now,
			    bool apply_cooldown, char *err, size_t errsz)
{
	MeAnalyticsEvent *event = &engine->active_event;
	uint32_t delta_in = engine->pending_delta_in;
	uint32_t delta_out = engine->pending_delta_out;
	int emitted;

	if (!engine->active)
		return 0;
	if (!increment_event_seq(event)) {
		event_set_err(err, errsz, "analytics event sequence is exhausted");
		return -1;
	}
	event->phase = ME_EVENT_PHASE_END;
	event->reason = reason;
	event_copy_observation_context(event, observation, person_count, delta_in,
				       delta_out);
	if (engine->pending_track_count > 0)
		event_copy_responsible_tracks(event, engine->pending_track_ids,
					       engine->pending_track_count);
	emitted = emit_event(engine, event);
	if (apply_cooldown && now.valid && engine->config.cooldown_ms > 0) {
		engine->cooldown_until_valid =
			time_add_ms(now.us, engine->config.cooldown_ms,
				     &engine->cooldown_until_us);
	}
	engine->active = false;
	engine->active_person_count = 0;
	clear_pending(engine);
	clear_rule_fact_states(engine);
	return emitted;
}

static void update_trigger_candidate(MeEventEngine *engine, bool condition,
					     MeTimeline now)
{
	if (!condition) {
		clear_trigger_candidate(engine);
		return;
	}
	if (!engine->trigger_pending) {
		engine->trigger_pending = true;
		engine->trigger_frames = 1;
		engine->trigger_since_us = now.us;
		engine->trigger_since_valid = now.valid;
	} else if (engine->trigger_frames < UINT32_MAX) {
		engine->trigger_frames++;
	}
}

static bool trigger_qualified(const MeEventEngine *engine, MeTimeline now)
{
	bool confirmed;
	uint64_t confirm_us = (uint64_t)engine->config.confirm_ms * 1000ULL;
	uint64_t debounce_us = (uint64_t)engine->config.debounce_ms * 1000ULL;

	if (!engine->trigger_pending)
		return false;
	confirmed =
		(engine->config.confirm_frames > 0 &&
		 engine->trigger_frames >= engine->config.confirm_frames) ||
		elapsed_at_least(now, engine->trigger_since_us,
				 engine->trigger_since_valid, confirm_us);
	if (!confirmed || !cooldown_elapsed(engine, now))
		return false;
	return debounce_us == 0 ||
	       elapsed_at_least(now, engine->trigger_since_us,
				engine->trigger_since_valid, debounce_us) ||
	       !now.valid;
}

static int process_accepted(MeEventEngine *engine,
				    const MeNormalizedObservation *observation,
				    char *err, size_t errsz)
{
	MeTimeline now = observation_timeline(engine, observation);
	MeRuleSummary summary;
	uint32_t person_count;
	bool condition;
	int emitted = 0;

	if (!engine->config.enabled) {
		engine->latest_person_count = 0;
		engine->latest_person_count_valid = true;
		clear_tracks(engine);
		clear_pending(engine);
		return 0;
	}

	person_count = update_tracks(engine, observation, now);
	engine->latest_person_count = person_count;
	engine->latest_person_count_valid = true;
	matching_rules(engine, observation, &summary);
	condition = engine->config.rule_type == ME_RULE_TYPE_OCCUPANCY
			    ? person_count > 0
			    : summary.match_count > 0;
	if (engine->active && engine->config.rule_type != ME_RULE_TYPE_OCCUPANCY &&
	    !remember_rule_facts(engine, &summary)) {
		event_set_err(err, errsz, "analytics rule fact state is exhausted");
		return -1;
	}

	if (!engine->active) {
		update_trigger_candidate(engine, condition, now);
		if (trigger_qualified(engine, now)) {
			int started = emit_start(engine, observation, &summary, person_count,
						 now, err, errsz);

			if (started < 0)
				return -1;
			emitted += started;
			clear_trigger_candidate(engine);
			engine->absent_since_valid = false;
		}
		return emitted;
	}

	if (!condition) {
		uint64_t absence_us =
			(uint64_t)engine->config.disappear_grace_ms * 1000ULL;

		if (!engine->absent_since_valid) {
			engine->absent_since_us = now.us;
			engine->absent_since_valid = now.valid;
		}
		if (absence_us == 0 ||
		    elapsed_at_least(now, engine->absent_since_us,
				     engine->absent_since_valid, absence_us)) {
			int ended = emit_end(engine, observation,
					     ME_EVENT_REASON_DISAPPEARED, person_count, now,
					     true, err, errsz);

			if (ended < 0)
				return -1;
			emitted += ended;
		}
		return emitted;
	}

	engine->absent_since_valid = false;
	if (engine->config.rule_type == ME_RULE_TYPE_OCCUPANCY) {
		if (person_count != engine->active_person_count)
			mark_update(engine, person_count, 0, 0, NULL, 0,
				    ME_EVENT_REASON_COUNT_CHANGED);
	} else if (summary.new_match_count > 0) {
		mark_update(engine, person_count, summary.delta_in, summary.delta_out,
				 summary.track_ids, summary.track_count,
				    ME_EVENT_REASON_DIRECTION);
	}
	emitted += maybe_emit_update(engine, observation, person_count, now, err,
				     errsz);
	return emitted;
}

int me_event_engine_init(MeEventEngine *engine,
				const MeAnalyticsConfig *config,
				uint64_t config_version, MeAnalyticsEventSink sink,
				void *sink_userdata, char *err, size_t errsz)
{
	if (!engine || !config) {
		event_set_err(err, errsz, "event engine and config are required");
		return -1;
	}
	if (me_analytics_config_validate(config, err, errsz) != 0)
		return -1;
	memset(engine, 0, sizeof(*engine));
	engine->config = *config;
	engine->config_version = config_version ? config_version : 1;
	engine->sink = sink;
	engine->sink_userdata = sink_userdata;
	me_observation_order_init(&engine->order);
	return 0;
}

int me_event_engine_deinit(MeEventEngine *engine, char *err, size_t errsz)
{
	int emitted = 0;

	if (!engine) {
		event_set_err(err, errsz, "event engine is required");
		return -1;
	}
	if (engine->active && engine->have_last_observation) {
		emitted = emit_end(engine, &engine->last_observation,
					ME_EVENT_REASON_PROCESS_RESTART,
					latest_person_count(engine),
					observation_timeline(engine,
							     &engine->last_observation),
					false, err, errsz);
		if (emitted < 0)
			return -1;
	}
	memset(engine, 0, sizeof(*engine));
	return emitted;
}

int me_event_engine_process(MeEventEngine *engine,
				    const MeNormalizedObservation *observation,
				    char *err, size_t errsz)
{
	MeObservationOrderResult order_result;
	MeObservationOrder next_order;
	int emitted = 0;
	int ended;
	int accepted;

	if (!engine || !observation) {
		event_set_err(err, errsz, "event engine and observation are required");
		return -1;
	}
	if (me_observation_validate(observation, err, errsz) != 0)
		return -1;
	if (engine->have_stream &&
	    strcmp(engine->channel_id, observation->channel_id) != 0) {
		if (engine->active && engine->have_last_observation) {
			ended = emit_end(engine, &engine->last_observation,
					 ME_EVENT_REASON_STREAM_RESET,
					 latest_person_count(engine),
					 observation_timeline(engine,
							     &engine->last_observation),
					 false, err, errsz);
			if (ended < 0)
				return -1;
			emitted += ended;
		}
		clear_runtime(engine);
		engine->have_stream = false;
		me_observation_order_init(&engine->order);
	}

	next_order = engine->order;
	order_result =
		me_observation_order_classify(&next_order, observation);
	if (order_result == ME_OBSERVATION_ORDER_INVALID) {
		event_set_err(err, errsz, "observation ordering input is invalid");
		return -1;
	}
	if (order_result == ME_OBSERVATION_ORDER_DUPLICATE ||
	    order_result == ME_OBSERVATION_ORDER_OUT_OF_ORDER)
		return emitted;
	if (order_result == ME_OBSERVATION_ORDER_EPOCH_ADVANCE) {
		if (engine->active && engine->have_last_observation) {
			ended = emit_end(engine, &engine->last_observation,
					 ME_EVENT_REASON_STREAM_RESET,
					 latest_person_count(engine),
					 observation_timeline(engine,
							     &engine->last_observation),
					 false, err, errsz);
				if (ended < 0)
					return -1;
				emitted += ended;
			}
			clear_runtime(engine);
		}
	engine->order = next_order;
	engine->have_stream = true;
	snprintf(engine->channel_id, sizeof(engine->channel_id), "%s",
		 observation->channel_id);
	engine->stream_epoch = observation->stream_epoch;
	engine->last_observation = *observation;
	engine->have_last_observation = true;
	accepted = process_accepted(engine, observation, err, errsz);
	if (accepted < 0)
		return -1;
	return emitted + accepted;
}

int me_event_engine_close(MeEventEngine *engine, MeEventReason reason,
				  char *err, size_t errsz)
{
	int emitted = 0;

	if (!engine || !valid_event_reason(reason)) {
		event_set_err(err, errsz, "invalid event engine close reason");
		return -1;
	}
	if (engine->active && engine->have_last_observation) {
		emitted = emit_end(engine, &engine->last_observation, reason,
				   latest_person_count(engine), observation_timeline(engine,
							    &engine->last_observation),
				   false, err, errsz);
		if (emitted < 0)
			return -1;
	}
	clear_runtime(engine);
	return emitted;
}

int me_event_engine_reconfigure(MeEventEngine *engine,
					const MeAnalyticsConfig *config,
					uint64_t config_version, char *err,
					size_t errsz)
{
	int emitted = 0;
	uint64_t next_version;
	MeEventReason reason;

	if (!engine || !config) {
		event_set_err(err, errsz, "event engine and config are required");
		return -1;
	}
	if (me_analytics_config_validate(config, err, errsz) != 0)
		return -1;
	if (config_version != 0) {
		if (config_version <= engine->config_version) {
			event_set_err(err, errsz,
				      "analytics config version must increase");
			return -1;
		}
		next_version = config_version;
	} else {
		if (engine->config_version == UINT64_MAX) {
			event_set_err(err, errsz,
				      "analytics config version is exhausted");
			return -1;
		}
		next_version = engine->config_version + 1;
	}
	reason = config->enabled ? ME_EVENT_REASON_RECONFIGURED
				 : ME_EVENT_REASON_ANALYTICS_DISABLED;
	if (engine->active && engine->have_last_observation) {
		emitted = emit_end(engine, &engine->last_observation, reason,
					latest_person_count(engine),
					observation_timeline(engine,
							     &engine->last_observation),
					false, err, errsz);
		if (emitted < 0)
			return -1;
	}
	clear_runtime(engine);
	engine->config = *config;
	engine->config_version = next_version;
	return emitted;
}

bool me_event_engine_is_active(const MeEventEngine *engine)
{
	return engine && engine->active;
}

const char *me_event_phase_name(MeEventPhase phase)
{
	switch (phase) {
	case ME_EVENT_PHASE_START:
		return "START";
	case ME_EVENT_PHASE_UPDATE:
		return "UPDATE";
	case ME_EVENT_PHASE_END:
		return "END";
	default:
		return "UNKNOWN";
	}
}

const char *me_event_reason_name(MeEventReason reason)
{
	switch (reason) {
	case ME_EVENT_REASON_NONE:
		return "none";
	case ME_EVENT_REASON_CONFIRMED:
		return "confirmed";
	case ME_EVENT_REASON_COUNT_CHANGED:
		return "count_changed";
	case ME_EVENT_REASON_DIRECTION:
		return "direction";
	case ME_EVENT_REASON_DISAPPEARED:
		return "disappeared";
	case ME_EVENT_REASON_STREAM_RESET:
		return "stream_reset";
	case ME_EVENT_REASON_RECONFIGURED:
		return "reconfigured";
	case ME_EVENT_REASON_ANALYTICS_DISABLED:
		return "analytics_disabled";
	case ME_EVENT_REASON_PROCESS_RESTART:
		return "process_restart";
	default:
		return "unknown";
	}
}
