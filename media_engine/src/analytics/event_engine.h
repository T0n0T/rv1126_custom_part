#ifndef ME_ANALYTICS_EVENT_ENGINE_H
#define ME_ANALYTICS_EVENT_ENGINE_H

#include "analytics/config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ME_ANALYTICS_EVENT_CONTRACT_VERSION 1U
#define ME_ANALYTICS_EVENT_ID_MAX 96U
#define ME_ANALYTICS_EVIDENCE_ID_MAX 96U
#define ME_ANALYTICS_EVENT_MAX_TRACKS ME_ANALYTICS_MAX_RULE_FACTS

typedef enum {
	ME_EVENT_PHASE_START = 1,
	ME_EVENT_PHASE_UPDATE = 2,
	ME_EVENT_PHASE_END = 3,
} MeEventPhase;

typedef enum {
	ME_EVENT_REASON_NONE = 0,
	ME_EVENT_REASON_CONFIRMED = 1,
	ME_EVENT_REASON_COUNT_CHANGED = 2,
	ME_EVENT_REASON_DIRECTION = 3,
	ME_EVENT_REASON_DISAPPEARED = 4,
	ME_EVENT_REASON_STREAM_RESET = 5,
	ME_EVENT_REASON_RECONFIGURED = 6,
	ME_EVENT_REASON_ANALYTICS_DISABLED = 7,
	ME_EVENT_REASON_PROCESS_RESTART = 8,
} MeEventReason;

/* Event records are synchronous callback values. A sink must copy a record if
 * it needs to retain it after the callback returns. */
typedef struct {
	uint32_t contract_version;
	char event_id[ME_ANALYTICS_EVENT_ID_MAX];
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	uint64_t stream_epoch;
	MeRuleType event_type;
	char rule_id[ME_ANALYTICS_RULE_ID_MAX];
	MeEventPhase phase;
	uint64_t event_seq;
	MeEventReason reason;
	int64_t event_time_us;
	MeClockState clock_state;
	int64_t source_pts;
	bool source_pts_valid;
	MeTimebase source_timebase;
	uint64_t frame_id;
	uint32_t person_count;
	uint32_t delta_in;
	uint32_t delta_out;
	/* For line/intrusion facts, zero means no target association was supplied. */
	uint32_t responsible_track_count;
	uint64_t responsible_track_ids[ME_ANALYTICS_EVENT_MAX_TRACKS];
	uint64_t config_version;
	char evidence_id[ME_ANALYTICS_EVIDENCE_ID_MAX];
} MeAnalyticsEvent;

typedef void (*MeAnalyticsEventSink)(void *userdata,
					    const MeAnalyticsEvent *event);

typedef struct {
	bool in_use;
	bool present;
	bool confirmed;
	uint64_t track_id;
	uint32_t seen_frames;
	uint64_t first_frame_id;
	int64_t first_timeline_us;
	bool first_timeline_valid;
	uint64_t last_seen_frame_id;
	int64_t last_timeline_us;
	bool last_timeline_valid;
	bool inside;
	bool inside_initialized;
	bool inside_candidate;
	bool inside_candidate_valid;
	int64_t inside_candidate_since_us;
} MeEventTrackState;

typedef struct {
	MeAnalyticsConfig config;
	uint64_t config_version;
	MeObservationOrder order;
	bool have_stream;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	uint64_t stream_epoch;
	MeNormalizedObservation last_observation;
	bool have_last_observation;

	MeEventTrackState tracks[ME_ANALYTICS_MAX_TRACKS];
	bool active;
	MeAnalyticsEvent active_event;
	uint32_t active_person_count;
	bool trigger_pending;
	uint32_t trigger_frames;
	int64_t trigger_since_us;
	bool trigger_since_valid;
	int64_t absent_since_us;
	bool absent_since_valid;
	int64_t last_rule_trigger_us;
	bool last_rule_trigger_valid;
	bool update_pending;
	uint32_t pending_person_count;
	uint32_t pending_delta_in;
	uint32_t pending_delta_out;
	uint32_t pending_track_count;
	uint64_t pending_track_ids[ME_ANALYTICS_EVENT_MAX_TRACKS];
	MeEventReason pending_reason;
	int64_t last_emit_timeline_us;
	bool last_emit_timeline_valid;
	int64_t cooldown_until_us;
	bool cooldown_until_valid;
	uint64_t next_event_serial;

	MeAnalyticsEventSink sink;
	void *sink_userdata;
} MeEventEngine;

int me_event_engine_init(MeEventEngine *engine,
				const MeAnalyticsConfig *config,
				uint64_t config_version, MeAnalyticsEventSink sink,
				void *sink_userdata, char *err, size_t errsz);

void me_event_engine_deinit(MeEventEngine *engine);

/* Processes one accepted observation. Returns the number of emitted records,
 * zero for duplicate/old observations, or -1 for invalid arguments/data.
 * The engine emits local START/UPDATE/END records regardless of the
 * send_updates/send_end delivery flags; those flags belong to the downstream
 * Alarm sink. */
int me_event_engine_process(MeEventEngine *engine,
				    const MeNormalizedObservation *observation,
				    char *err, size_t errsz);

/* Ends the active event using the latest observation, if one exists, and
 * clears all candidates/tracks. A reset does not impose cooldown. */
int me_event_engine_close(MeEventEngine *engine, MeEventReason reason,
				  char *err, size_t errsz);

/* Validates and installs a new rule configuration. Active events are ended
 * with RECONFIGURED before the new version takes effect. */
int me_event_engine_reconfigure(MeEventEngine *engine,
					const MeAnalyticsConfig *config,
					uint64_t config_version, char *err,
					size_t errsz);

bool me_event_engine_is_active(const MeEventEngine *engine);

const char *me_event_phase_name(MeEventPhase phase);
const char *me_event_reason_name(MeEventReason reason);

#endif /* ME_ANALYTICS_EVENT_ENGINE_H */
