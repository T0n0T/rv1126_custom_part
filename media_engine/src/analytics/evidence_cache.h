#ifndef ME_ANALYTICS_EVIDENCE_CACHE_H
#define ME_ANALYTICS_EVIDENCE_CACHE_H

#include "analytics/event_engine.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ME_EVIDENCE_SHA256_MAX 65U
#define ME_EVIDENCE_PATH_MAX 256U
#define ME_EVIDENCE_STATE_MAX 32U

typedef struct MeEvidenceCache MeEvidenceCache;

typedef enum {
	ME_EVIDENCE_ACCURACY_NONE = 0,
	ME_EVIDENCE_ACCURACY_EXACT = 1,
	ME_EVIDENCE_ACCURACY_APPROXIMATE = 2,
} MeEvidenceAccuracy;

typedef struct {
	char evidence_id[ME_ANALYTICS_EVIDENCE_ID_MAX];
	char event_id[ME_ANALYTICS_EVENT_ID_MAX];
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	uint64_t stream_epoch;
	uint64_t frame_id;
	bool source_pts_valid;
	int64_t source_pts;
	MeTimebase source_timebase;
	int64_t capture_time_us;
	uint32_t width;
	uint32_t height;
	uint64_t bytes;
	char sha256[ME_EVIDENCE_SHA256_MAX];
	MeEvidenceAccuracy accuracy;
	uint64_t frame_delta;
	int64_t pts_delta;
	int64_t created_at_us;
	int64_t expires_at_us;
	char storage_state[ME_EVIDENCE_STATE_MAX];
	char delivery_state[ME_EVIDENCE_STATE_MAX];
	char path[ME_EVIDENCE_PATH_MAX];
} MeEvidenceRecord;

/* Opens or recovers a directory-backed cache. JPEG files and metadata are
 * published independently but metadata is written last, so a recovered
 * record always points at a complete JPEG. */
MeEvidenceCache *me_evidence_cache_open(const char *directory,
						uint64_t max_bytes,
						uint32_t retention_s, char *err,
						size_t errsz);
void me_evidence_cache_close(MeEvidenceCache *cache);

/* Adds a frame-to-PTS mapping from the analytics path. It is deliberately
 * independent from JPEG encoding because the encoder may run ahead of or
 * behind RockIVA callbacks. */
int me_evidence_cache_note_frame(MeEvidenceCache *cache,
						 const char *channel_id, uint64_t stream_epoch,
						 uint64_t frame_id, bool source_pts_valid,
						 int64_t source_pts, MeTimebase source_timebase,
						 uint32_t width,
					 uint32_t height);

/* Adds one encoded JPEG to the bounded in-memory recent-frame window. The
 * event capture operation later chooses exact or nearest evidence from this
 * window and publishes it durably. */
int me_evidence_cache_push_jpeg(MeEvidenceCache *cache,
					const uint8_t *jpeg, size_t jpeg_size,
					bool source_pts_valid, int64_t source_pts);

/* Captures the best frame for an event, atomically publishes JPEG plus
 * metadata, and fills event->evidence_id. For UPDATE/END this only attaches
 * an already-published START evidence record. */
int me_evidence_cache_capture(MeEvidenceCache *cache, MeAnalyticsEvent *event,
					  MeEvidenceRecord *record, char *err,
					  size_t errsz);

int me_evidence_cache_lookup(MeEvidenceCache *cache, const char *evidence_id,
					 MeEvidenceRecord *record);
uint64_t me_evidence_cache_bytes(MeEvidenceCache *cache);
uint64_t me_evidence_cache_count(MeEvidenceCache *cache);
uint64_t me_evidence_cache_prune(MeEvidenceCache *cache);

const char *me_evidence_accuracy_name(MeEvidenceAccuracy accuracy);

#endif /* ME_ANALYTICS_EVIDENCE_CACHE_H */
