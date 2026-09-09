/* Host-side deterministic tests for the durable analytics event journal. */

#include "analytics/event_journal.h"

#include <glib.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

static char *new_journal_path(void)
{
	char template[] = "/tmp/me_event_journal_test_XXXXXX";
	int fd = mkstemp(template);

	if (fd < 0)
		return NULL;
	close(fd);
	unlink(template);
	return g_strdup(template);
}

static MeAnalyticsEvent make_event(MeEventPhase phase, uint64_t sequence)
{
	MeAnalyticsEvent event;

	memset(&event, 0, sizeof(event));
	event.contract_version = ME_ANALYTICS_EVENT_CONTRACT_VERSION;
	snprintf(event.event_id, sizeof(event.event_id), "event-1");
	snprintf(event.channel_id, sizeof(event.channel_id), "channel-1");
	event.stream_epoch = 1;
	event.event_type = ME_RULE_TYPE_OCCUPANCY;
	snprintf(event.rule_id, sizeof(event.rule_id), "people-flow");
	event.phase = phase;
	event.event_seq = sequence;
	event.reason = phase == ME_EVENT_PHASE_START
		? ME_EVENT_REASON_CONFIRMED
		: (phase == ME_EVENT_PHASE_END ? ME_EVENT_REASON_DISAPPEARED
									 : ME_EVENT_REASON_COUNT_CHANGED);
	event.event_time_us = 1000000 + (int64_t)sequence * 100000;
	event.clock_state = ME_CLOCK_STATE_SYNCED;
	event.source_pts_valid = true;
	event.source_pts = (int64_t)sequence * 9000;
	event.source_timebase.num = 1;
	event.source_timebase.den = 90000;
	event.frame_id = sequence;
	event.person_count = phase == ME_EVENT_PHASE_END ? 0 : 1;
	event.responsible_track_count = 1;
	event.responsible_track_ids[0] = 77;
	event.config_version = 1;
	return event;
}

static int append_event(MeEventJournal *journal, MeEventPhase phase,
						uint64_t sequence, uint64_t *cursor)
{
	MeAnalyticsEvent event = make_event(phase, sequence);
	char err[256] = {0};

	return me_event_journal_append(journal, &event, cursor, err, sizeof(err));
}

static void test_round_trip_and_recovery(void)
{
	char *path = new_journal_path();
	MeEventJournal *journal;
	GPtrArray *records;
	MeEventJournalRecord *record;
	uint64_t cursor = 0;
	uint64_t oldest = 0;
	uint64_t latest = 0;
	bool gap = false;

	CHECK(path != NULL, "journal test path allocated");
	if (!path)
		return;
	journal = me_event_journal_open(path, 16, 65536, NULL, 0);
	CHECK(journal != NULL, "journal opens empty");
	if (!journal)
		goto out;
	CHECK(append_event(journal, ME_EVENT_PHASE_START, 1, &cursor) == 0 &&
			cursor == 1, "START is appended with cursor 1");
	CHECK(append_event(journal, ME_EVENT_PHASE_UPDATE, 2, &cursor) == 0 &&
			cursor == 2, "UPDATE is appended with cursor 2");
	CHECK(append_event(journal, ME_EVENT_PHASE_END, 3, &cursor) == 0 &&
			cursor == 3, "END is appended with cursor 3");
	CHECK(me_event_journal_latest_cursor(journal) == 3,
		  "journal tail reports cursor 3");
	records = me_event_journal_snapshot_after(journal, 0, &gap, &oldest,
									 &latest);
	CHECK(records != NULL && records->len == 3, "snapshot returns all records");
	CHECK(!gap && oldest == 1 && latest == 3,
		  "complete snapshot has no replay gap");
	if (records && records->len == 3) {
		record = g_ptr_array_index(records, 1);
		CHECK(record->cursor == 2 && record->event.phase == ME_EVENT_PHASE_UPDATE,
		      "snapshot preserves cursor and event payload");
	}
	if (records)
		g_ptr_array_free(records, TRUE);
	me_event_journal_close(journal);

	journal = me_event_journal_open(path, 16, 65536, NULL, 0);
	CHECK(journal != NULL, "journal recovers after restart");
	if (journal) {
		CHECK(me_event_journal_latest_cursor(journal) == 3,
		      "recovered journal preserves cursor tail");
		records = me_event_journal_snapshot_after(journal, 1, &gap, &oldest,
									 &latest);
		CHECK(records != NULL && records->len == 2,
		      "after_cursor replays only later records");
		CHECK(!gap && oldest == 1 && latest == 3,
		      "contiguous replay has no gap");
		if (records)
			g_ptr_array_free(records, TRUE);
		me_event_journal_close(journal);
	}
out:
	unlink(path);
	g_free(path);
}

static void test_corrupt_tail_recovery(void)
{
	char *path = new_journal_path();
	MeEventJournal *journal;
	FILE *file;
	GPtrArray *records;
	uint64_t cursor = 0;
	bool gap = false;

	CHECK(path != NULL, "corrupt-tail test path allocated");
	if (!path)
		return;
	journal = me_event_journal_open(path, 8, 65536, NULL, 0);
	CHECK(journal != NULL, "corrupt-tail journal opens");
	if (!journal)
		goto out;
	CHECK(append_event(journal, ME_EVENT_PHASE_START, 1, &cursor) == 0,
	      "corrupt-tail START is durable");
	CHECK(append_event(journal, ME_EVENT_PHASE_UPDATE, 2, &cursor) == 0,
	      "corrupt-tail UPDATE is durable");
	me_event_journal_close(journal);

	file = fopen(path, "a");
	CHECK(file != NULL, "corrupt tail file opens");
	if (file) {
		fputs("{\"v\":1,\"cursor\":99", file);
		fclose(file);
	}

	journal = me_event_journal_open(path, 8, 65536, NULL, 0);
	CHECK(journal != NULL, "journal truncates incomplete tail on restart");
	if (journal) {
		CHECK(me_event_journal_latest_cursor(journal) == 2,
		      "tail recovery keeps last complete cursor");
		records = me_event_journal_snapshot_after(journal, 0, &gap, NULL, NULL);
		CHECK(records != NULL && records->len == 2,
		      "tail recovery keeps complete records only");
		if (records)
			g_ptr_array_free(records, TRUE);
		CHECK(append_event(journal, ME_EVENT_PHASE_END, 3, &cursor) == 0 &&
				cursor == 3, "append after tail recovery continues cursor sequence");
		me_event_journal_close(journal);
	}
out:
	unlink(path);
	g_free(path);
}

static void test_pressure_and_boundary_protection(void)
{
	char *path = new_journal_path();
	MeEventJournal *journal;
	GPtrArray *records;
	MeEventJournalRecord *record;
	uint64_t cursor = 0;
	bool gap = false;

	CHECK(path != NULL, "pressure test path allocated");
	if (!path)
		return;
	journal = me_event_journal_open(path, 1, 65536, NULL, 0);
	CHECK(journal != NULL, "pressure journal opens");
	if (!journal)
		goto out;
	CHECK(append_event(journal, ME_EVENT_PHASE_START, 1, &cursor) == 0,
	      "pressure START is retained");
	cursor = 0;
	CHECK(append_event(journal, ME_EVENT_PHASE_UPDATE, 2, &cursor) == 1 &&
			cursor == 0 && me_event_journal_dropped_updates(journal) == 1,
		  "UPDATE is dropped when only lifecycle boundary fits");
	cursor = 0;
	CHECK(append_event(journal, ME_EVENT_PHASE_END, 3, &cursor) == -1 &&
			cursor == 0, "END is rejected instead of evicting START");
	records = me_event_journal_snapshot_after(journal, 0, &gap, NULL, NULL);
	CHECK(records != NULL && records->len == 1, "boundary pressure keeps START");
	if (records) {
		record = g_ptr_array_index(records, 0);
		CHECK(record->event.phase == ME_EVENT_PHASE_START,
		      "retained boundary is START");
		g_ptr_array_free(records, TRUE);
	}
	me_event_journal_close(journal);
	unlink(path);

	journal = me_event_journal_open(path, 2, 65536, NULL, 0);
	CHECK(journal != NULL, "compaction journal opens");
	if (journal) {
		CHECK(append_event(journal, ME_EVENT_PHASE_START, 1, &cursor) == 0,
		      "compaction START is appended");
		CHECK(append_event(journal, ME_EVENT_PHASE_UPDATE, 2, &cursor) == 0,
		      "compaction UPDATE is appended");
		CHECK(append_event(journal, ME_EVENT_PHASE_END, 3, &cursor) == 0,
		      "compaction evicts UPDATE to protect END");
		records = me_event_journal_snapshot_after(journal, 0, &gap, NULL, NULL);
		CHECK(records != NULL && records->len == 2 && gap,
		      "evicted UPDATE is reported as a replay gap");
		if (records && records->len == 2) {
			record = g_ptr_array_index(records, 1);
			CHECK(record->cursor == 3 && record->event.phase == ME_EVENT_PHASE_END,
			      "compaction retains START and END cursors");
		}
		if (records)
			g_ptr_array_free(records, TRUE);
		cursor = 0;
		CHECK(append_event(journal, ME_EVENT_PHASE_START, 4, &cursor) == -1 &&
				cursor == 0, "full lifecycle boundaries are never evicted");
		me_event_journal_close(journal);
	}
out:
	unlink(path);
	g_free(path);
}

int main(void)
{
	test_round_trip_and_recovery();
	test_corrupt_tail_recovery();
	test_pressure_and_boundary_protection();
	if (failures) {
		printf("event_journal: %d test(s) failed\n", failures);
		return 1;
	}
	printf("event_journal: all tests passed\n");
	return 0;
}
