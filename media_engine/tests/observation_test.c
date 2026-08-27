/* Host-side contract tests for normalized analytics observations. */

#include "analytics/observation.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, name)                                                     \
	do {                                                                      \
		if (cond)                                                           \
			printf("[PASS] %s\n", name);                                  \
		else {                                                              \
			printf("[FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__);      \
			failures++;                                                       \
		}                                                                   \
	} while (0)

static MeNormalizedObservation valid_observation(void)
{
	MeNormalizedObservation observation;

	me_observation_init(&observation);
	snprintf(observation.channel_id, sizeof(observation.channel_id),
	         "35020000001310000001");
	observation.stream_epoch = 7;
	observation.frame_id = 42;
	observation.source_pts = 4200000000LL;
	observation.source_pts_valid = true;
	observation.capture_time_us = 1760000000123456LL;
	observation.clock_state = ME_CLOCK_STATE_SYNCED;
	observation.frame_width = 640;
	observation.frame_height = 360;
	snprintf(observation.backend_name, sizeof(observation.backend_name),
	         "rockiva");
	snprintf(observation.backend_version, sizeof(observation.backend_version),
	         "sdk-test");
	snprintf(observation.model_version, sizeof(observation.model_version),
	         "model-test");
	observation.track_count = 1;
	observation.tracks[0].track_id = 11;
	observation.tracks[0].class_id = ME_TRACK_CLASS_PERSON;
	observation.tracks[0].state = ME_TRACK_STATE_ACTIVE;
	observation.tracks[0].bbox.left = 1000;
	observation.tracks[0].bbox.top = 2000;
	observation.tracks[0].bbox.right = 5000;
	observation.tracks[0].bbox.bottom = 9000;
	observation.tracks[0].score_q = 8750;
	observation.rule_count = 1;
	snprintf(observation.rules[0].rule_id, sizeof(observation.rules[0].rule_id),
	         "main-roi");
	observation.rules[0].rule_type = ME_RULE_TYPE_OCCUPANCY;
	observation.rules[0].direction = ME_RULE_DIRECTION_NONE;
	observation.rules[0].score_q = 9000;
	return observation;
}

int main(void)
{
	MeNormalizedObservation observation = valid_observation();
	MeObservationOrder order;
	char err[256];

	CHECK(me_observation_validate(&observation, err, sizeof(err)) == 0,
	      "valid observation accepted");
	CHECK(observation.contract_version == ME_ANALYTICS_CONTRACT_VERSION,
	      "contract version initialized");
	CHECK(observation.source_timebase.num == 1 &&
	          observation.source_timebase.den == 1000000000U,
	      "source pts timebase is explicit");

	observation.tracks[0].bbox.right = ME_ANALYTICS_COORD_SCALE + 1;
	CHECK(me_observation_validate(&observation, err, sizeof(err)) != 0,
	      "bbox outside normalized range rejected");
	observation = valid_observation();
	observation.source_pts_valid = true;
	observation.source_timebase.den = 0;
	CHECK(me_observation_validate(&observation, err, sizeof(err)) != 0,
	      "invalid pts timebase rejected");
	observation = valid_observation();
	observation.clock_state = ME_CLOCK_STATE_UNAVAILABLE;
	observation.capture_time_us = 1;
	CHECK(me_observation_validate(&observation, err, sizeof(err)) != 0,
	      "unavailable clock cannot carry capture time");

	observation = valid_observation();
	observation.rules[0].track_id = 11;
	observation.rules[1] = observation.rules[0];
	observation.rules[1].track_id = 12;
	observation.rule_count = 2;
	CHECK(me_observation_validate(&observation, err, sizeof(err)) == 0,
	      "same rule id may describe distinct responsible tracks");
	observation.rules[1].track_id = 11;
	CHECK(me_observation_validate(&observation, err, sizeof(err)) != 0,
	      "duplicate rule fact for one track is rejected");

	observation = valid_observation();
	me_observation_order_init(&order);
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_FIRST,
	      "first observation accepted");
	observation.frame_id++;
	observation.source_pts++;
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_ACCEPTED,
	      "next frame accepted");
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_DUPLICATE,
	      "duplicate frame identified");
	observation.frame_id--;
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_OUT_OF_ORDER,
	      "older frame identified");
	observation = valid_observation();
	observation.stream_epoch++;
	observation.frame_id = 1;
	observation.source_pts = 1;
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_EPOCH_ADVANCE,
	      "new stream epoch accepted");
	observation.stream_epoch--;
	CHECK(me_observation_order_classify(&order, &observation) ==
	          ME_OBSERVATION_ORDER_OUT_OF_ORDER,
	      "old stream epoch rejected");

	if (failures == 0)
		printf("\nobservation: all tests passed\n");
	else
		printf("\nobservation: %d test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
