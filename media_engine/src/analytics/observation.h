#ifndef ME_ANALYTICS_OBSERVATION_H
#define ME_ANALYTICS_OBSERVATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ME_ANALYTICS_CONTRACT_VERSION 2U
#define ME_ANALYTICS_COORD_SCALE 10000U
#define ME_ANALYTICS_CHANNEL_ID_MAX 64U
#define ME_ANALYTICS_BACKEND_NAME_MAX 32U
#define ME_ANALYTICS_VERSION_MAX 64U
#define ME_ANALYTICS_MODEL_VERSION_MAX 64U
#define ME_ANALYTICS_RULE_ID_MAX 64U
#define ME_ANALYTICS_MAX_TRACKS 64U
#define ME_ANALYTICS_MAX_RULE_FACTS 16U

typedef enum {
	ME_CLOCK_STATE_UNAVAILABLE = 0,
	ME_CLOCK_STATE_SYNCED = 1,
	ME_CLOCK_STATE_ESTIMATED = 2,
	ME_CLOCK_STATE_INVALID = 3,
} MeClockState;

typedef enum {
	ME_TRACK_CLASS_UNKNOWN = 0,
	ME_TRACK_CLASS_PERSON = 1,
	ME_TRACK_CLASS_OTHER = 2,
} MeTrackClass;

typedef enum {
	ME_TRACK_STATE_UNKNOWN = 0,
	ME_TRACK_STATE_NEW = 1,
	ME_TRACK_STATE_ACTIVE = 2,
	ME_TRACK_STATE_LOST = 3,
	ME_TRACK_STATE_REMOVED = 4,
} MeTrackState;

typedef enum {
	ME_RULE_TYPE_UNKNOWN = 0,
	ME_RULE_TYPE_OCCUPANCY = 1,
	ME_RULE_TYPE_LINE_CROSS = 2,
	ME_RULE_TYPE_INTRUSION = 3,
} MeRuleType;

typedef enum {
	ME_RULE_DIRECTION_NONE = 0,
	ME_RULE_DIRECTION_IN = 1,
	ME_RULE_DIRECTION_OUT = 2,
} MeRuleDirection;

typedef struct {
	uint32_t num;
	uint32_t den;
} MeTimebase;

/* Coordinates are normalized to [0, ME_ANALYTICS_COORD_SCALE]. */
typedef struct {
	uint32_t left;
	uint32_t top;
	uint32_t right;
	uint32_t bottom;
} MeNormalizedBBox;

typedef struct {
	uint64_t track_id;
	MeTrackClass class_id;
	MeTrackState state;
	MeNormalizedBBox bbox;
	uint32_t score_q;
} MeTrackObservation;

typedef struct {
	char rule_id[ME_ANALYTICS_RULE_ID_MAX];
	MeRuleType rule_type;
	MeRuleDirection direction;
	uint32_t score_q;
	/* Zero denotes a rule-level fact; otherwise this is the responsible
	 * backend track in the current stream epoch. */
	uint64_t track_id;
} MeRuleFact;

/* Backend-neutral result for exactly one source frame. */
typedef struct {
	uint32_t contract_version;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	uint64_t stream_epoch;
	uint64_t frame_id;
	int64_t source_pts;
	bool source_pts_valid;
	MeTimebase source_timebase;
	int64_t capture_time_us;
	MeClockState clock_state;
	uint32_t frame_width;
	uint32_t frame_height;
	char backend_name[ME_ANALYTICS_BACKEND_NAME_MAX];
	char backend_version[ME_ANALYTICS_VERSION_MAX];
	char model_version[ME_ANALYTICS_MODEL_VERSION_MAX];
	uint32_t track_count;
	MeTrackObservation tracks[ME_ANALYTICS_MAX_TRACKS];
	uint32_t rule_count;
	MeRuleFact rules[ME_ANALYTICS_MAX_RULE_FACTS];
} MeNormalizedObservation;

typedef enum {
	ME_OBSERVATION_ORDER_INVALID = 0,
	ME_OBSERVATION_ORDER_FIRST = 1,
	ME_OBSERVATION_ORDER_ACCEPTED = 2,
	ME_OBSERVATION_ORDER_DUPLICATE = 3,
	ME_OBSERVATION_ORDER_OUT_OF_ORDER = 4,
	ME_OBSERVATION_ORDER_EPOCH_ADVANCE = 5,
} MeObservationOrderResult;

/* Keeps only the latest accepted observation's ordering keys. */
typedef struct {
	bool initialized;
	uint64_t stream_epoch;
	uint64_t frame_id;
	int64_t source_pts;
	bool source_pts_valid;
} MeObservationOrder;

void me_observation_init(MeNormalizedObservation *observation);
int me_observation_validate(const MeNormalizedObservation *observation,
				    char *err, size_t errsz);

void me_observation_order_init(MeObservationOrder *order);
MeObservationOrderResult me_observation_order_classify(
	MeObservationOrder *order, const MeNormalizedObservation *observation);

const char *me_clock_state_name(MeClockState state);
const char *me_track_state_name(MeTrackState state);
const char *me_rule_type_name(MeRuleType type);
const char *me_rule_direction_name(MeRuleDirection direction);

#endif /* ME_ANALYTICS_OBSERVATION_H */
