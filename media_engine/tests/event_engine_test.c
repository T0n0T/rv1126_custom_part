/* Host-side deterministic tests for the analytics event state machine. */

#include "analytics/event_engine.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, name)                                                     \
	do {                                                                        \
		if (cond)                                                               \
			printf("[PASS] %s\n", name);                                      \
		else {                                                                  \
			printf("[FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__);          \
			failures++;                                                           \
		}                                                                       \
	} while (0)

#define MAX_CAPTURED_EVENTS 32

typedef struct {
	MeAnalyticsEvent events[MAX_CAPTURED_EVENTS];
	unsigned int count;
} EventCapture;

static void capture_event(void *userdata, const MeAnalyticsEvent *event)
{
	EventCapture *capture = userdata;

	if (capture->count < MAX_CAPTURED_EVENTS)
		capture->events[capture->count++] = *event;
}

static MeAnalyticsConfig test_config(MeRuleType type)
{
	MeAnalyticsConfig config;

	me_analytics_config_defaults(&config);
	config.enabled = true;
	snprintf(config.model, sizeof(config.model), "probe-model");
	config.width = 640;
	config.height = 360;
	config.fps = 10;
	config.score_threshold_q = 5000;
	config.confirm_frames = 2;
	config.confirm_ms = 1000;
	config.debounce_ms = 100;
	config.disappear_grace_ms = 200;
	config.cooldown_ms = 300;
	config.update_interval_ms = 100;
	config.rule_type = type;
	snprintf(config.rule_id, sizeof(config.rule_id),
	         type == ME_RULE_TYPE_OCCUPANCY ? "occupancy" : "entrance");
	config.roi_enabled = true;
	config.roi.left = 1000;
	config.roi.top = 1000;
	config.roi.right = 9000;
	config.roi.bottom = 9000;
	config.line_enabled = type == ME_RULE_TYPE_LINE_CROSS;
	config.line_x1 = 5000;
	config.line_y1 = 1000;
	config.line_x2 = 5000;
	config.line_y2 = 9000;
	return config;
}

static MeNormalizedObservation make_observation(uint64_t epoch,
						uint64_t frame_id,
						int64_t capture_time_us)
{
	MeNormalizedObservation observation;

	me_observation_init(&observation);
	snprintf(observation.channel_id, sizeof(observation.channel_id), "channel-1");
	observation.stream_epoch = epoch;
	observation.frame_id = frame_id;
	observation.source_pts = (capture_time_us - 1000000) * 1000;
	observation.source_pts_valid = true;
	observation.capture_time_us = capture_time_us;
	observation.clock_state = ME_CLOCK_STATE_SYNCED;
	observation.frame_width = 640;
	observation.frame_height = 360;
	snprintf(observation.backend_name, sizeof(observation.backend_name), "test");
	snprintf(observation.backend_version, sizeof(observation.backend_version),
	         "host");
	snprintf(observation.model_version, sizeof(observation.model_version),
	         "probe-model");
	return observation;
}

static void add_person(MeNormalizedObservation *observation, uint64_t track_id,
			       MeTrackState state, uint32_t left)
{
	MeTrackObservation *track =
		&observation->tracks[observation->track_count++];

	track->track_id = track_id;
	track->class_id = ME_TRACK_CLASS_PERSON;
	track->state = state;
	track->bbox.left = left;
	track->bbox.top = 2000;
	track->bbox.right = left + 1000;
	track->bbox.bottom = 5000;
	track->score_q = 9000;
}

static void add_rule(MeNormalizedObservation *observation, const char *rule_id,
			     MeRuleType type, MeRuleDirection direction,
			     uint64_t track_id)
{
	MeRuleFact *rule = &observation->rules[observation->rule_count++];

	snprintf(rule->rule_id, sizeof(rule->rule_id), "%s", rule_id);
	rule->rule_type = type;
	rule->direction = direction;
	rule->score_q = 9000;
	rule->track_id = track_id;
}

static void assert_event(const EventCapture *capture, unsigned int index,
				 MeEventPhase phase, MeEventReason reason,
				 uint64_t seq)
{
	const MeAnalyticsEvent *event = &capture->events[index];

	CHECK(event->phase == phase, "event phase matches");
	CHECK(event->reason == reason, "event reason matches");
	CHECK(event->event_seq == seq, "event sequence is monotonic");
	CHECK(event->contract_version == ME_ANALYTICS_EVENT_CONTRACT_VERSION,
	      "event contract version initialized");
	CHECK(event->event_id[0] != '\0', "event id is present");
}

static void test_occupancy_lifecycle(void)
{
	MeAnalyticsConfig config = test_config(ME_RULE_TYPE_OCCUPANCY);
	MeEventEngine engine;
	EventCapture capture = {0};
	MeNormalizedObservation observation;
	char err[256];
	int rc;

	CHECK(me_event_engine_init(&engine, &config, 4, capture_event, &capture, err,
	                          sizeof(err)) == 0,
	      "occupancy engine initializes");

	observation = make_observation(1, 1, 1000000);
	add_person(&observation, 11, ME_TRACK_STATE_NEW, 2000);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 0 && capture.count == 0, "first person frame remains candidate");

	observation = make_observation(1, 2, 1100000);
	add_person(&observation, 11, ME_TRACK_STATE_ACTIVE, 2200);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 0 && capture.count == 0,
	      "track confirmation does not immediately emit event");

	observation = make_observation(1, 3, 1200000);
	add_person(&observation, 11, ME_TRACK_STATE_ACTIVE, 2400);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 1, "confirmed occupancy emits START");
	assert_event(&capture, 0, ME_EVENT_PHASE_START, ME_EVENT_REASON_CONFIRMED, 1);
	CHECK(capture.events[0].person_count == 1 &&
	              capture.events[0].source_pts_valid &&
	              capture.events[0].event_time_us == 1200000,
	      "START carries count and source timing");

	observation = make_observation(1, 4, 1300000);
	add_person(&observation, 11, ME_TRACK_STATE_ACTIVE, 2600);
	add_person(&observation, 12, ME_TRACK_STATE_NEW, 4000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "new second track still awaits confirmation");

	observation = make_observation(1, 5, 1400000);
	add_person(&observation, 11, ME_TRACK_STATE_ACTIVE, 2800);
	add_person(&observation, 12, ME_TRACK_STATE_ACTIVE, 4200);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 2, "count change emits UPDATE");
	assert_event(&capture, 1, ME_EVENT_PHASE_UPDATE,
	             ME_EVENT_REASON_COUNT_CHANGED, 2);
	CHECK(capture.events[1].person_count == 2, "UPDATE carries latest occupancy");

	observation = make_observation(1, 6, 1500000);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 0 && capture.count == 2, "absence starts grace period");
	observation = make_observation(1, 7, 1800000);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 3, "grace expiry emits END");
	assert_event(&capture, 2, ME_EVENT_PHASE_END, ME_EVENT_REASON_DISAPPEARED, 3);
	CHECK(capture.events[2].person_count == 0,
	      "disappearance END reports zero current occupancy");
	CHECK(!me_event_engine_is_active(&engine), "engine is inactive after END");

	/* Candidate confirmation before cooldown expiry must wait, then start. */
	observation = make_observation(1, 8, 1900000);
	add_person(&observation, 13, ME_TRACK_STATE_NEW, 3000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "post-END candidate begins during cooldown");
	observation = make_observation(1, 9, 2000000);
	add_person(&observation, 13, ME_TRACK_STATE_ACTIVE, 3200);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "cooldown suppresses early START");
	observation = make_observation(1, 10, 2100000);
	add_person(&observation, 13, ME_TRACK_STATE_ACTIVE, 3400);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 4, "START resumes after cooldown");
	CHECK(strcmp(capture.events[0].event_id, capture.events[3].event_id) != 0,
	      "new lifecycle receives a new event id");

	/* Duplicate and old frames are ignored without changing the lifecycle. */
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0 &&
	              capture.count == 4,
	      "duplicate observation ignored");
	observation.frame_id = 9;
	observation.source_pts -= 100000;
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0 &&
	              capture.count == 4,
	      "out-of-order observation ignored");

	/* A new stream epoch closes the old lifecycle before accepting new IDs. */
	observation = make_observation(2, 1, 3000000);
	add_person(&observation, 21, ME_TRACK_STATE_NEW, 3000);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 5,
	      "stream epoch change closes previous lifecycle");
	CHECK(capture.events[4].phase == ME_EVENT_PHASE_END &&
	              capture.events[4].reason == ME_EVENT_REASON_STREAM_RESET &&
	              capture.events[4].stream_epoch == 1 &&
	              capture.events[4].person_count == 1,
	      "stream reset END retains old epoch context");
	observation = make_observation(2, 2, 3100000);
	add_person(&observation, 21, ME_TRACK_STATE_ACTIVE, 3200);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "new epoch starts with fresh track confirmation");
	observation = make_observation(2, 3, 3150000);
	add_person(&observation, 21, ME_TRACK_STATE_ACTIVE, 3400);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "new epoch candidate still needs event confirmation");
	observation = make_observation(2, 4, 3300000);
	add_person(&observation, 21, ME_TRACK_STATE_ACTIVE, 3600);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1 &&
	              capture.count == 6,
	      "new epoch emits an independent START");

	CHECK(me_event_engine_close(&engine, ME_EVENT_REASON_PROCESS_RESTART, err,
	                           sizeof(err)) == 1 &&
	              capture.count == 7,
	      "explicit process restart closes active event");
	CHECK(capture.events[6].reason == ME_EVENT_REASON_PROCESS_RESTART,
	      "restart reason is preserved");
	me_event_engine_deinit(&engine);
}

static void test_line_crossing_and_reconfigure(void)
{
	MeAnalyticsConfig line = test_config(ME_RULE_TYPE_LINE_CROSS);
	MeAnalyticsConfig occupancy = test_config(ME_RULE_TYPE_OCCUPANCY);
	MeEventEngine engine;
	EventCapture capture = {0};
	MeNormalizedObservation observation;
	char err[256];
	int rc;

	line.confirm_frames = 1;
	line.confirm_ms = 1000;
	line.disappear_grace_ms = 100;
	line.cooldown_ms = 0;
	CHECK(me_event_engine_init(&engine, &line, 5, capture_event, &capture, err,
	                          sizeof(err)) == 0,
	      "line-cross engine initializes");

	observation = make_observation(1, 1, 10000000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 0);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "line hit waits for debounce");
	observation = make_observation(1, 2, 10100000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 0);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 1, "debounced line hit emits START");
	assert_event(&capture, 0, ME_EVENT_PHASE_START, ME_EVENT_REASON_CONFIRMED, 1);
	CHECK(capture.events[0].delta_in == 1 && capture.events[0].delta_out == 0,
	      "line START carries inbound delta");

	observation = make_observation(1, 3, 10150000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 0);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "line debounce suppresses repeated hit");
	observation = make_observation(1, 4, 10250000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 0);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 2, "accepted line hit emits UPDATE");
	assert_event(&capture, 1, ME_EVENT_PHASE_UPDATE, ME_EVENT_REASON_DIRECTION, 2);
	CHECK(capture.events[1].delta_in == 1, "line UPDATE carries inbound delta");

	observation = make_observation(1, 5, 10450000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "line absence starts grace period");
	observation = make_observation(1, 6, 10650000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1 &&
	              capture.count == 3,
	      "line grace expiry emits END");
	CHECK(capture.events[2].reason == ME_EVENT_REASON_DISAPPEARED,
	      "line END reason is disappearance");

	/* Reconfiguration ends an active lifecycle before installing new semantics. */
	line.confirm_frames = 1;
	observation = make_observation(1, 7, 11000000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_OUT, 0);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "second line candidate begins");
	observation = make_observation(1, 8, 11100000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_OUT, 0);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1,
	      "second line candidate starts");
	occupancy.confirm_frames = 1;
	occupancy.debounce_ms = 0;
	rc = me_event_engine_reconfigure(&engine, &occupancy, 9, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 5, "reconfigure closes active event");
	CHECK(capture.events[4].phase == ME_EVENT_PHASE_END &&
	              capture.events[4].reason == ME_EVENT_REASON_RECONFIGURED &&
	              capture.events[4].config_version == 5,
	      "reconfigure END keeps old config version");
	CHECK(!me_event_engine_is_active(&engine),
	      "reconfigure leaves engine ready for new rule");

	/* Disabling analytics also closes an active lifecycle explicitly. */
	observation = make_observation(1, 9, 11200000);
	add_person(&observation, 31, ME_TRACK_STATE_NEW, 3000);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 &&
	              capture.count == 6,
	      "reconfigured occupancy emits START");
	{
		MeAnalyticsConfig disabled = occupancy;

		disabled.enabled = false;
		rc = me_event_engine_reconfigure(&engine, &disabled, 10, err,
		                                  sizeof(err));
	}
	CHECK(rc == 1 && capture.count == 7,
	      "disabling analytics closes active event");
	CHECK(capture.events[6].reason == ME_EVENT_REASON_ANALYTICS_DISABLED,
	      "analytics disabled reason is preserved");
	me_event_engine_deinit(&engine);
}

static void test_line_scope_and_roi_debounce(void)
{
	MeAnalyticsConfig line = test_config(ME_RULE_TYPE_LINE_CROSS);
	MeAnalyticsConfig occupancy = test_config(ME_RULE_TYPE_OCCUPANCY);
	MeEventEngine engine;
	EventCapture capture = {0};
	MeNormalizedObservation observation;
	char err[256];
	int rc;

	line.confirm_frames = 1;
	line.confirm_ms = 1000;
	line.debounce_ms = 100;
	line.disappear_grace_ms = 0;
	CHECK(me_event_engine_init(&engine, &line, 12, capture_event, &capture, err,
	                          sizeof(err)) == 0,
	      "multi-target line engine initializes");

	observation = make_observation(1, 1, 20000000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 101);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 102);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "multi-target line facts enter debounce");
	observation = make_observation(1, 2, 20100000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 101);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 102);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 1,
	      "multi-target line facts emit one grouped START");
	CHECK(capture.events[0].delta_in == 2 &&
	              capture.events[0].responsible_track_count == 2 &&
	              capture.events[0].responsible_track_ids[0] == 101 &&
	              capture.events[0].responsible_track_ids[1] == 102,
	      "START preserves all inbound responsible tracks");

	observation = make_observation(1, 3, 20200000);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_IN, 101);
	add_rule(&observation, "entrance", ME_RULE_TYPE_LINE_CROSS,
	         ME_RULE_DIRECTION_OUT, 102);
	rc = me_event_engine_process(&engine, &observation, err, sizeof(err));
	CHECK(rc == 1 && capture.count == 2,
	      "grouped direction change emits UPDATE");
	CHECK(capture.events[1].delta_in == 1 &&
	              capture.events[1].delta_out == 1 &&
	              capture.events[1].responsible_track_count == 2,
	      "UPDATE preserves direction deltas and responsible tracks");

	observation = make_observation(1, 4, 20300000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1 &&
	              capture.count == 3 &&
              capture.events[2].phase == ME_EVENT_PHASE_END &&
              capture.events[2].responsible_track_count == 2 &&
              capture.events[2].responsible_track_ids[0] == 101 &&
              capture.events[2].responsible_track_ids[1] == 102,
      "line END retains the last responsible tracks");
	me_event_engine_deinit(&engine);
	memset(&capture, 0, sizeof(capture));

	occupancy.confirm_frames = 1;
	occupancy.confirm_ms = 1000;
	occupancy.debounce_ms = 100;
	occupancy.disappear_grace_ms = 500;
	occupancy.cooldown_ms = 0;
	CHECK(me_event_engine_init(&engine, &occupancy, 13, capture_event, &capture, err,
	                          sizeof(err)) == 0,
	      "ROI debounce engine initializes");

	observation = make_observation(1, 1, 30000000);
	add_person(&observation, 31, ME_TRACK_STATE_NEW, 2000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "ROI occupancy starts confirmation");
	observation = make_observation(1, 2, 30100000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 2200);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1,
	      "ROI occupancy emits START after debounce");

	observation = make_observation(1, 3, 30200000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 2400);
	add_person(&observation, 32, ME_TRACK_STATE_NEW, 9000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "outside track does not change ROI count");
	observation = make_observation(1, 4, 30250000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 2600);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 9000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "ROI boundary candidate waits for debounce");
	observation = make_observation(1, 5, 30350000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 2800);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 4000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "ROI entry begins hysteresis after target confirmation");
	observation = make_observation(1, 6, 30450000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 3000);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 4200);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1 &&
	              capture.events[capture.count - 1].phase == ME_EVENT_PHASE_UPDATE &&
	              capture.events[capture.count - 1].person_count == 2,
	      "ROI entry updates count only after debounce");

	observation = make_observation(1, 7, 30500000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 3200);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 9000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "ROI exit candidate does not immediately change count");
	observation = make_observation(1, 8, 30550000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 3400);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 9000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 0,
	      "brief ROI boundary exit is absorbed");
	observation = make_observation(1, 9, 30650000);
	add_person(&observation, 31, ME_TRACK_STATE_ACTIVE, 3600);
	add_person(&observation, 32, ME_TRACK_STATE_ACTIVE, 9000);
	CHECK(me_event_engine_process(&engine, &observation, err, sizeof(err)) == 1 &&
	              capture.events[capture.count - 1].person_count == 1,
	      "ROI exit updates count after debounce");
	me_event_engine_deinit(&engine);
}

int main(void)
{
	test_occupancy_lifecycle();
	test_line_crossing_and_reconfigure();
	test_line_scope_and_roi_debounce();
	if (failures == 0)
		printf("\nevent_engine: all tests passed\n");
	else
		printf("\nevent_engine: %d test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
