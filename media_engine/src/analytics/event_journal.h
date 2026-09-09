#ifndef ME_ANALYTICS_EVENT_JOURNAL_H
#define ME_ANALYTICS_EVENT_JOURNAL_H

#include "analytics/event_engine.h"

#include <glib.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ME_EVENT_JOURNAL_VERSION 1U

typedef struct MeEventJournal MeEventJournal;

typedef struct {
	uint64_t cursor;
	MeAnalyticsEvent event;
} MeEventJournalRecord;

/* Returns NULL when the journal cannot be opened or recovered. */
MeEventJournal *me_event_journal_open(const char *path, uint32_t max_records,
					      uint64_t max_bytes, char *err,
					      size_t errsz);
void me_event_journal_close(MeEventJournal *journal);

/* Returns 0 when durable, 1 when an UPDATE was dropped under pressure, and
 * -1 when a lifecycle boundary could not be made durable. */
int me_event_journal_append(MeEventJournal *journal,
					const MeAnalyticsEvent *event, uint64_t *cursor,
					char *err, size_t errsz);

/* Returns copies of records whose cursor is greater than after_cursor. The
 * caller owns the returned array and its records. */
GPtrArray *me_event_journal_snapshot_after(
		MeEventJournal *journal, uint64_t after_cursor, bool *replay_gap,
		uint64_t *oldest_cursor, uint64_t *latest_cursor);

uint64_t me_event_journal_latest_cursor(MeEventJournal *journal);
uint64_t me_event_journal_dropped_updates(MeEventJournal *journal);

#endif /* ME_ANALYTICS_EVENT_JOURNAL_H */
