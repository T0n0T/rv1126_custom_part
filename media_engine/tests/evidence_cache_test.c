#include "analytics/evidence_cache.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <stdint.h>
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

static char *new_cache_directory(void)
{
	char template[] = "/tmp/me_evidence_cache_test_XXXXXX";

	return g_mkdtemp(template) ? g_strdup(template) : NULL;
}

static MeAnalyticsEvent make_event(const char *event_id, uint64_t frame_id,
						   int64_t source_pts)
{
	MeAnalyticsEvent event;

	memset(&event, 0, sizeof(event));
	event.contract_version = ME_ANALYTICS_EVENT_CONTRACT_VERSION;
	g_strlcpy(event.event_id, event_id, sizeof(event.event_id));
	g_strlcpy(event.channel_id, "channel-1", sizeof(event.channel_id));
	event.stream_epoch = 7;
	event.event_type = ME_RULE_TYPE_OCCUPANCY;
	g_strlcpy(event.rule_id, "people-flow", sizeof(event.rule_id));
	event.phase = ME_EVENT_PHASE_START;
	event.event_seq = 1;
	event.reason = ME_EVENT_REASON_CONFIRMED;
	event.event_time_us = 1234567;
	event.clock_state = ME_CLOCK_STATE_SYNCED;
	event.source_pts_valid = true;
	event.source_pts = source_pts;
	event.source_timebase.num = 1;
	event.source_timebase.den = 1000000000;
	event.frame_id = frame_id;
	event.person_count = 2;
	return event;
}

static void cleanup_directory(const char *directory, const char *id1,
						 const char *id2)
{
	gchar *path;

	if (!directory)
		return;
	if (id1) {
		path = g_build_filename(directory, id1, ".jpg", NULL);
		g_remove(path);
		g_free(path);
		path = g_build_filename(directory, id1, ".json", NULL);
		g_remove(path);
		g_free(path);
	}
	if (id2) {
		path = g_build_filename(directory, id2, ".jpg", NULL);
		g_remove(path);
		g_free(path);
		path = g_build_filename(directory, id2, ".json", NULL);
		g_remove(path);
		g_free(path);
	}
	g_rmdir(directory);
}

static void test_exact_capture_and_restart(void)
{
	static const uint8_t jpeg[] = {0xff, 0xd8, 0x01, 0x02, 0xff, 0xd9};
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	MeAnalyticsEvent event;
	MeEvidenceRecord record;
	char err[256] = {0};
	gchar *contents = NULL;
	gsize length = 0;

	CHECK(directory != NULL, "evidence cache directory allocated");
	if (!directory)
		return;
	cache = me_evidence_cache_open(directory, 65536, 3600, err,
							 sizeof(err));
	CHECK(cache != NULL, "evidence cache opens");
	if (!cache)
		goto out;
	event = make_event("event-exact", 10, 100);
	CHECK(me_evidence_cache_note_frame(cache, "channel-1", 7, 10, true,
									 100, event.source_timebase, 640, 360) == 0,
				 "exact frame mapping is recorded");
	CHECK(me_evidence_cache_push_jpeg(cache, jpeg, sizeof(jpeg), true, 100) == 0,
				 "JPEG enters recent frame cache");
	CHECK(me_evidence_cache_capture(cache, &event, &record, err,
							 sizeof(err)) == 0,
			 "exact event evidence is published");
	CHECK(event.evidence_id[0] != '\0' && record.accuracy == ME_EVIDENCE_ACCURACY_EXACT &&
			record.frame_id == 10 && record.frame_delta == 0,
			 "exact evidence preserves frame identity");
	CHECK(g_file_get_contents(record.path, &contents, &length, NULL) &&
			length == sizeof(jpeg) && !memcmp(contents, jpeg, sizeof(jpeg)),
			 "published JPEG is complete");
	g_free(contents);
	contents = NULL;
	me_evidence_cache_close(cache);

	cache = me_evidence_cache_open(directory, 65536, 3600, err,
							 sizeof(err));
	CHECK(cache != NULL && me_evidence_cache_count(cache) == 1,
			 "metadata and JPEG recover after restart");
	if (cache) {
		MeEvidenceRecord recovered;
		CHECK(me_evidence_cache_lookup(cache, event.evidence_id, &recovered) == 0 &&
				recovered.bytes == sizeof(jpeg) &&
				!strcmp(recovered.sha256, record.sha256) &&
				recovered.source_timebase.num == 1 &&
				recovered.source_timebase.den == 1000000000U &&
				!strcmp(recovered.storage_state, "ready") &&
				!strcmp(recovered.delivery_state, "awaiting_consumer"),
				"recovered metadata keeps checksum and size");
		me_evidence_cache_close(cache);
	}
out:
	g_free(contents);
	cleanup_directory(directory, "ev-", NULL);
	/* The generated evidence ID is hash-based; remove any remaining files. */
	if (directory) {
		GDir *dir = g_dir_open(directory, 0, NULL);
		if (dir) {
			const gchar *name;
			while ((name = g_dir_read_name(dir)) != NULL) {
				gchar *path = g_build_filename(directory, name, NULL);
				g_remove(path);
				g_free(path);
			}
			g_dir_close(dir);
		}
		g_rmdir(directory);
	}
	g_free(directory);
}

static void test_unannotated_jpeg_is_not_exact(void)
{
	static const uint8_t jpeg[] = {0xff, 0xd8, 0x31, 0xff, 0xd9};
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	MeAnalyticsEvent event;
	char err[256] = {0};

	CHECK(directory != NULL, "unannotated cache directory allocated");
	if (!directory)
		return;
	cache = me_evidence_cache_open(directory, 65536, 3600, err,
							 sizeof(err));
	CHECK(cache != NULL, "unannotated cache opens");
	if (cache) {
		event = make_event("event-unannotated", 40, 400);
		CHECK(me_evidence_cache_push_jpeg(cache, jpeg, sizeof(jpeg), true, 400) == 0,
				 "unannotated JPEG enters recent frame cache");
		CHECK(me_evidence_cache_capture(cache, &event, NULL, err,
							 sizeof(err)) == 1 && me_evidence_cache_count(cache) == 0,
				 "unannotated JPEG cannot be reported as exact evidence");
		me_evidence_cache_close(cache);
	}
	if (directory) {
		GDir *dir = g_dir_open(directory, 0, NULL);
		if (dir) {
			const gchar *name;
			while ((name = g_dir_read_name(dir)) != NULL) {
				gchar *path = g_build_filename(directory, name, NULL);
				g_remove(path);
				g_free(path);
			}
			g_dir_close(dir);
		}
		g_rmdir(directory);
	}
	g_free(directory);
}

static void test_approximate_fallback_and_capacity(void)
{
	static const uint8_t jpeg1[] = {0xff, 0xd8, 0x11, 0xff, 0xd9};
	static const uint8_t jpeg2[] = {0xff, 0xd8, 0x22, 0xff, 0xd9};
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	MeAnalyticsEvent event;
	MeEvidenceRecord record;
	char err[256] = {0};

	CHECK(directory != NULL, "approximate cache directory allocated");
	if (!directory)
		return;
	cache = me_evidence_cache_open(directory, sizeof(jpeg1), 3600, err,
							 sizeof(err));
	CHECK(cache != NULL, "small evidence cache opens");
	if (!cache)
		goto out;
	event = make_event("event-approx", 30, 220);
	me_evidence_cache_note_frame(cache, "channel-1", 7, 20, true, 200,
								 event.source_timebase, 640, 360);
	me_evidence_cache_push_jpeg(cache, jpeg1, sizeof(jpeg1), true, 200);
	CHECK(me_evidence_cache_capture(cache, &event, &record, err,
							 sizeof(err)) == 0,
			 "nearest frame is accepted as approximate evidence");
	CHECK(record.accuracy == ME_EVIDENCE_ACCURACY_APPROXIMATE &&
			record.frame_delta == 10 && record.pts_delta == -20,
			 "approximate evidence records frame and PTS deltas");
	me_evidence_cache_push_jpeg(cache, jpeg2, sizeof(jpeg2), true, 300);
	CHECK(me_evidence_cache_bytes(cache) == sizeof(jpeg1),
			 "cache remains within byte limit");
	me_evidence_cache_close(cache);
out:
	if (directory) {
		GDir *dir = g_dir_open(directory, 0, NULL);
		if (dir) {
			const gchar *name;
			while ((name = g_dir_read_name(dir)) != NULL) {
				gchar *path = g_build_filename(directory, name, NULL);
				g_remove(path);
				g_free(path);
			}
			g_dir_close(dir);
		}
		g_rmdir(directory);
	}
	g_free(directory);
}

static void test_rejects_unsafe_id(void)
{
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	MeAnalyticsEvent event;
	char err[256] = {0};

	CHECK(directory != NULL, "unsafe-id cache directory allocated");
	if (!directory)
		return;
	cache = me_evidence_cache_open(directory, 65536, 3600, err,
							 sizeof(err));
	CHECK(cache != NULL, "unsafe-id cache opens");
	if (cache) {
		event = make_event("event-unsafe", 1, 1);
		g_strlcpy(event.evidence_id, "../escape", sizeof(event.evidence_id));
		CHECK(me_evidence_cache_capture(cache, &event, NULL, err,
								 sizeof(err)) < 0,
				"path traversal evidence id is rejected");
		me_evidence_cache_close(cache);
	}
	cleanup_directory(directory, NULL, NULL);
	g_free(directory);
}

static void test_pts_conflict_is_approximate(void)
{
	static const uint8_t jpeg[] = {0xff, 0xd8, 0x41, 0xff, 0xd9};
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	MeAnalyticsEvent event;
	MeEvidenceRecord record;
	char err[256] = {0};

	CHECK(directory != NULL, "PTS conflict cache directory allocated");
	if (!directory)
		return;
	cache = me_evidence_cache_open(directory, 65536, 3600, err,
						 sizeof(err));
	CHECK(cache != NULL, "PTS conflict cache opens");
	if (!cache)
		goto out;
	event = make_event("event-pts-conflict", 10, 101);
	CHECK(me_evidence_cache_note_frame(cache, "channel-1", 7, 10, true,
								 100, event.source_timebase, 640, 360) == 0,
			  "PTS conflict frame mapping is recorded");
	CHECK(me_evidence_cache_push_jpeg(cache, jpeg, sizeof(jpeg), true, 100) == 0,
			  "PTS conflict JPEG enters recent frame cache");
	CHECK(me_evidence_cache_capture(cache, &event, &record, err,
						 sizeof(err)) == 0 &&
				record.accuracy == ME_EVIDENCE_ACCURACY_APPROXIMATE &&
				record.frame_delta == 0 && record.pts_delta == -1,
			  "valid PTS conflict cannot be reported as exact by frame ID");
	me_evidence_cache_close(cache);
out:
	if (directory) {
		GDir *dir = g_dir_open(directory, 0, NULL);
		if (dir) {
			const gchar *name;
			while ((name = g_dir_read_name(dir)) != NULL) {
				gchar *path = g_build_filename(directory, name, NULL);
				g_remove(path);
				g_free(path);
			}
			g_dir_close(dir);
		}
		g_rmdir(directory);
	}
	g_free(directory);
}

static void test_cleans_orphan_files_and_temps(void)
{
	static const char *const names[] = {
		"orphan.jpg",
		"orphan.json",
		"ev-orphan.jpg.tmp-ABC123",
		"ev-orphan.json.tmp-DEF456",
	};
	char *directory = new_cache_directory();
	MeEvidenceCache *cache;
	char err[256] = {0};
	unsigned i;

	CHECK(directory != NULL, "orphan cleanup directory allocated");
	if (!directory)
		return;
	for (i = 0; i < G_N_ELEMENTS(names); i++) {
		gchar *path = g_build_filename(directory, names[i], NULL);
		CHECK(g_file_set_contents(path, "orphan", -1, NULL),
			  "orphan fixture created");
		g_free(path);
	}
	cache = me_evidence_cache_open(directory, 65536, 3600, err,
						 sizeof(err));
	CHECK(cache != NULL, "orphan cleanup cache opens");
	if (cache) {
		for (i = 0; i < G_N_ELEMENTS(names); i++) {
			gchar *path = g_build_filename(directory, names[i], NULL);
			CHECK(!g_file_test(path, G_FILE_TEST_EXISTS),
			      "orphan evidence file removed on open");
			g_free(path);
		}
		CHECK(me_evidence_cache_count(cache) == 0,
			  "orphan cleanup leaves no recovered records");
		me_evidence_cache_close(cache);
	}
	g_rmdir(directory);
	g_free(directory);
}

int main(void)
{
	test_exact_capture_and_restart();
	test_unannotated_jpeg_is_not_exact();
	test_approximate_fallback_and_capacity();
	test_rejects_unsafe_id();
	test_pts_conflict_is_approximate();
	test_cleans_orphan_files_and_temps();
	if (failures) {
		printf("evidence_cache: %d test(s) failed\n", failures);
		return 1;
	}
	printf("evidence_cache: all tests passed\n");
	return 0;
}
