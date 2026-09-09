#include "analytics/event_journal.h"

#include "analytics/event_codec.h"
#include "common/util.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ME_EVENT_JOURNAL_MAX_JSON_INTEGER 9007199254740991.0

typedef struct {
	MeEventJournalRecord public_record;
	uint64_t bytes;
} JournalRecord;

struct MeEventJournal {
	GMutex lock;
	GQueue *records; /* JournalRecord */
	FILE *file;
	char *path;
	uint32_t max_records;
	uint64_t max_bytes;
	uint64_t bytes;
	uint64_t next_cursor;
	uint64_t dropped_updates;
};

static void journal_set_err(char *err, size_t errsz, const char *message)
{
	if (err && errsz > 0)
		snprintf(err, errsz, "%s", message ? message : "event journal error");
}

static bool json_u64(const cJSON *object, const char *key, uint64_t *out)
{
	const cJSON *value;
	double number;
	char *end = NULL;
	unsigned long long parsed;

	if (!object || !key || !out)
		return false;
	value = cJSON_GetObjectItemCaseSensitive(object, key);
	if (cJSON_IsNumber(value)) {
		number = value->valuedouble;
		if (number < 0 || number != floor(number) ||
		    number > ME_EVENT_JOURNAL_MAX_JSON_INTEGER)
			return false;
		*out = (uint64_t)number;
		return true;
	}
	if (!cJSON_IsString(value) || !value->valuestring ||
	    !value->valuestring[0] || value->valuestring[0] < '0' ||
	    value->valuestring[0] > '9')
		return false;
	errno = 0;
	parsed = strtoull(value->valuestring, &end, 10);
	if (errno || !end || *end != '\0')
		return false;
	*out = (uint64_t)parsed;
	return true;
}

static JournalRecord *record_from_line(const char *line, size_t len,
					       char *err, size_t errsz)
{
	JournalRecord *record = NULL;
	cJSON *root = NULL;
	cJSON *event_object;
	uint64_t version;

	root = cJSON_ParseWithLength(line, len);
	if (!root || !cJSON_IsObject(root)) {
		journal_set_err(err, errsz, "event journal line is not an object");
		goto out;
	}
	if (!json_u64(root, "v", &version) || version != ME_EVENT_JOURNAL_VERSION ||
	    !json_u64(root, "cursor", &version) || version == 0) {
		journal_set_err(err, errsz, "event journal line has invalid version/cursor");
		goto out;
	}
	record = g_new0(JournalRecord, 1);
	record->public_record.cursor = version;
	event_object = cJSON_GetObjectItemCaseSensitive(root, "event");
	if (!event_object ||
	    me_analytics_event_from_json(event_object, &record->public_record.event,
	                                  err, errsz) != 0) {
		g_free(record);
		record = NULL;
		goto out;
	}
	record->bytes = len;

out:
	cJSON_Delete(root);
	return record;
}

static char *record_to_line(uint64_t cursor, const MeAnalyticsEvent *event,
					uint64_t *bytes)
{
	cJSON *root = NULL;
	cJSON *event_object = NULL;
	char *text = NULL;
	char *line = NULL;

	if (!event || !bytes)
		return NULL;
	root = cJSON_CreateObject();
	event_object = me_analytics_event_to_json(event);
	if (!root || !event_object)
		goto out;
	cJSON_AddNumberToObject(root, "v", ME_EVENT_JOURNAL_VERSION);
	cJSON_AddNumberToObject(root, "cursor", (double)cursor);
	cJSON_AddItemToObject(root, "event", event_object);
	event_object = NULL;
	text = cJSON_PrintUnformatted(root);
	if (!text)
		goto out;
	line = g_strdup_printf("%s\n", text);
	if (line)
		*bytes = strlen(line);

out:
	cJSON_free(text);
	cJSON_Delete(event_object);
	cJSON_Delete(root);
	return line;
}

static int ensure_parent_directory(const char *path, char *err, size_t errsz)
{
	char *parent;
	char *slash;

	parent = g_strdup(path);
	if (!parent) {
		journal_set_err(err, errsz, "event journal path allocation failed");
		return -1;
	}
	slash = strrchr(parent, '/');
	if (!slash) {
		g_free(parent);
		return 0;
	}
	if (slash == parent)
		slash[1] = '\0';
	else
		*slash = '\0';
	if (parent[0] && g_mkdir_with_parents(parent, 0700) != 0) {
		journal_set_err(err, errsz, "create event journal directory failed");
		g_free(parent);
		return -1;
	}
	g_free(parent);
	return 0;
}

static JournalRecord *oldest_update(GPtrArray *records)
{
	guint i;

	for (i = 0; i < records->len; i++) {
		JournalRecord *record = g_ptr_array_index(records, i);
		if (record->public_record.event.phase == ME_EVENT_PHASE_UPDATE)
			return record;
	}
	return NULL;
}

static int write_records(FILE *file, GPtrArray *records, uint64_t *bytes,
					char *err, size_t errsz)
{
	uint64_t total = 0;
	guint i;

	if (!file || !records || !bytes)
		return -1;
	for (i = 0; i < records->len; i++) {
		JournalRecord *record = g_ptr_array_index(records, i);
		uint64_t line_bytes = 0;
		char *line = record_to_line(record->public_record.cursor,
								&record->public_record.event, &line_bytes);
		if (!line || fwrite(line, 1, (size_t)line_bytes, file) !=
					(size_t)line_bytes) {
			journal_set_err(err, errsz, "write event journal compaction failed");
			g_free(line);
			return -1;
		}
		record->bytes = line_bytes;
		total += line_bytes;
		g_free(line);
	}
	*bytes = total;
	return 0;
}

static int rewrite_locked(MeEventJournal *journal, GPtrArray *records,
					  uint64_t *bytes, char *err, size_t errsz)
{
	char *tmp_path;
	FILE *tmp;
	int fd;
	int saved_errno;

	if (!journal || !records || !bytes)
		return -1;
	tmp_path = g_strdup_printf("%s.tmp.%ld", journal->path, (long)getpid());
	if (!tmp_path) {
		journal_set_err(err, errsz, "event journal temporary path allocation failed");
		return -1;
	}
	fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		journal_set_err(err, errsz, "open event journal temporary file failed");
		g_free(tmp_path);
		return -1;
	}
	tmp = fdopen(fd, "w");
	if (!tmp) {
		saved_errno = errno;
		close(fd);
		unlink(tmp_path);
		journal_set_err(err, errsz, "fdopen event journal temporary file failed");
		errno = saved_errno;
		g_free(tmp_path);
		return -1;
	}
	if (write_records(tmp, records, bytes, err, errsz) != 0 ||
		fflush(tmp) != 0 || fsync(fileno(tmp)) != 0) {
		unlink(tmp_path);
		fclose(tmp);
		g_free(tmp_path);
		return -1;
	}
	if (fclose(tmp) != 0) {
		unlink(tmp_path);
		g_free(tmp_path);
		journal_set_err(err, errsz, "close event journal temporary file failed");
		return -1;
	}
	if (rename(tmp_path, journal->path) != 0) {
		journal_set_err(err, errsz, "replace event journal failed");
		unlink(tmp_path);
		g_free(tmp_path);
		return -1;
	}
	if (journal->file)
		fclose(journal->file);
	journal->file = fopen(journal->path, "a+");
	if (!journal->file || fseek(journal->file, 0, SEEK_END) != 0) {
		journal_set_err(err, errsz, "reopen event journal failed");
		g_free(tmp_path);
		return -1;
	}
	chmod(journal->path, 0600);
	g_free(tmp_path);
	return 0;
}

static int compact_to_fit_locked(MeEventJournal *journal, uint64_t incoming_bytes,
						MeEventPhase incoming_phase, char *err,
						size_t errsz)
{
	GPtrArray *keep;
	GPtrArray *drop;
	uint64_t keep_bytes;
	JournalRecord *candidate;
	GQueue *new_queue;
	GQueue *old_queue;
	uint64_t rewritten_bytes;
	guint i;

	if (journal->records->length + 1 <= journal->max_records &&
	    journal->bytes <= journal->max_bytes &&
	    incoming_bytes <= journal->max_bytes - journal->bytes)
		return 0;
	if (incoming_bytes > journal->max_bytes) {
		if (incoming_phase == ME_EVENT_PHASE_UPDATE) {
			journal->dropped_updates++;
			return 1;
		}
		journal_set_err(err, errsz, "event journal lifecycle record exceeds byte limit");
		return -1;
	}

	keep = g_ptr_array_new();
	drop = g_ptr_array_new();
	if (!keep || !drop) {
		g_ptr_array_free(keep, TRUE);
		g_ptr_array_free(drop, TRUE);
		journal_set_err(err, errsz, "event journal compaction allocation failed");
		return -1;
	}
	keep_bytes = 0;
	for (GList *it = journal->records->head; it; it = it->next) {
		JournalRecord *record = it->data;
		g_ptr_array_add(keep, record);
		keep_bytes += record->bytes;
	}
	while (keep->len + 1 > journal->max_records ||
	       keep_bytes > journal->max_bytes ||
	       incoming_bytes > journal->max_bytes - keep_bytes) {
		candidate = oldest_update(keep);
		if (!candidate) {
			if (incoming_phase == ME_EVENT_PHASE_UPDATE) {
				journal->dropped_updates++;
				g_ptr_array_free(keep, TRUE);
				g_ptr_array_free(drop, TRUE);
				return 1;
			}
			journal_set_err(err, errsz,
							"event journal full: lifecycle boundary protected");
			g_ptr_array_free(keep, TRUE);
			g_ptr_array_free(drop, TRUE);
			return -1;
		}
		g_ptr_array_remove(keep, candidate);
		g_ptr_array_add(drop, candidate);
		keep_bytes -= candidate->bytes;
	}

	rewritten_bytes = 0;
	if (rewrite_locked(journal, keep, &rewritten_bytes, err, errsz) != 0) {
		g_ptr_array_free(keep, TRUE);
		g_ptr_array_free(drop, TRUE);
		return -1;
	}
	new_queue = g_queue_new();
	if (!new_queue) {
		journal_set_err(err, errsz, "event journal queue allocation failed");
		g_ptr_array_free(keep, TRUE);
		g_ptr_array_free(drop, TRUE);
		return -1;
	}
	for (i = 0; i < keep->len; i++)
		g_queue_push_tail(new_queue, g_ptr_array_index(keep, i));
	for (i = 0; i < drop->len; i++)
		g_free(g_ptr_array_index(drop, i));
	old_queue = journal->records;
	journal->records = new_queue;
	journal->bytes = rewritten_bytes;
	g_queue_free(old_queue);
	g_ptr_array_free(keep, TRUE);
	g_ptr_array_free(drop, TRUE);
	return 0;
}

MeEventJournal *me_event_journal_open(const char *path, uint32_t max_records,
					      uint64_t max_bytes, char *err,
					      size_t errsz)
{
	MeEventJournal *journal;
	char *line = NULL;
	size_t line_cap = 0;
	ssize_t line_len;
	off_t good_offset = 0;
	bool corrupt = false;
	uint64_t last_cursor = 0;

	if (!path || !path[0] || max_records == 0 || max_bytes == 0) {
		journal_set_err(err, errsz, "event journal path and limits are required");
		return NULL;
	}
	if (ensure_parent_directory(path, err, errsz) != 0)
		return NULL;
	journal = g_new0(MeEventJournal, 1);
	if (!journal) {
		journal_set_err(err, errsz, "event journal allocation failed");
		return NULL;
	}
	g_mutex_init(&journal->lock);
	journal->records = g_queue_new();
	journal->path = g_strdup(path);
	journal->max_records = max_records;
	journal->max_bytes = max_bytes;
	journal->file = fopen(path, "a+");
	if (!journal->records || !journal->path || !journal->file) {
		journal_set_err(err, errsz, "open event journal failed");
		me_event_journal_close(journal);
		return NULL;
	}
	chmod(path, 0600);
	if (fseek(journal->file, 0, SEEK_SET) != 0) {
		journal_set_err(err, errsz, "seek event journal failed");
		me_event_journal_close(journal);
		return NULL;
	}
	while ((line_len = getline(&line, &line_cap, journal->file)) >= 0) {
		JournalRecord *record;
		char parse_err[128] = {0};
		off_t next_offset;

		if (line_len == 0 || line[line_len - 1] != '\n') {
			corrupt = true;
			break;
		}
		record = record_from_line(line, (size_t)line_len, parse_err,
							   sizeof(parse_err));
		next_offset = ftello(journal->file);
		if (!record || record->public_record.cursor <= last_cursor) {
			g_free(record);
			corrupt = true;
			break;
		}
		g_queue_push_tail(journal->records, record);
		journal->bytes += record->bytes;
		last_cursor = record->public_record.cursor;
		good_offset = next_offset;
	}
	g_free(line);
	if (ferror(journal->file)) {
		journal_set_err(err, errsz, "read event journal failed");
		me_event_journal_close(journal);
		return NULL;
	}
	if (corrupt) {
		if (ftruncate(fileno(journal->file), good_offset) != 0) {
			journal_set_err(err, errsz, "truncate corrupt event journal failed");
			me_event_journal_close(journal);
			return NULL;
		}
		me_log(ME_LOG_WARN, "event journal truncated after corrupt tail: %s", path);
	}
	if (fseek(journal->file, 0, SEEK_END) != 0) {
		journal_set_err(err, errsz, "seek event journal end failed");
		me_event_journal_close(journal);
		return NULL;
	}
	journal->next_cursor = last_cursor;
	if (journal->records->length > journal->max_records ||
	    journal->bytes > journal->max_bytes) {
		GPtrArray *all = g_ptr_array_new();
		uint64_t rewritten_bytes = 0;
		if (!all) {
			journal_set_err(err, errsz, "event journal recovery allocation failed");
			me_event_journal_close(journal);
			return NULL;
		}
		for (GList *it = journal->records->head; it; it = it->next)
			g_ptr_array_add(all, it->data);
		while (all->len > journal->max_records ||
		       (journal->bytes > journal->max_bytes && all->len > 0)) {
			JournalRecord *candidate = oldest_update(all);
			if (!candidate) {
				journal_set_err(err, errsz,
								"event journal recovery cannot protect boundaries");
				g_ptr_array_free(all, TRUE);
				me_event_journal_close(journal);
				return NULL;
			}
			g_ptr_array_remove(all, candidate);
			journal->bytes -= candidate->bytes;
			g_queue_remove(journal->records, candidate);
			g_free(candidate);
		}
		if (rewrite_locked(journal, all, &rewritten_bytes, err, errsz) != 0) {
			g_ptr_array_free(all, TRUE);
			me_event_journal_close(journal);
			return NULL;
		}
		journal->bytes = rewritten_bytes;
		g_ptr_array_free(all, TRUE);
	}
	return journal;
}

void me_event_journal_close(MeEventJournal *journal)
{
	if (!journal)
		return;
	if (journal->file)
		fclose(journal->file);
	if (journal->records) {
		while (!g_queue_is_empty(journal->records))
			g_free(g_queue_pop_head(journal->records));
		g_queue_free(journal->records);
	}
	g_free(journal->path);
	g_mutex_clear(&journal->lock);
	g_free(journal);
}

int me_event_journal_append(MeEventJournal *journal,
					const MeAnalyticsEvent *event, uint64_t *cursor,
					char *err, size_t errsz)
{
	JournalRecord *record;
	char *line;
	uint64_t next_cursor;
	uint64_t line_bytes = 0;
	off_t old_end = -1;
	int rc;

	if (cursor)
		*cursor = 0;
	if (!journal || !event) {
		journal_set_err(err, errsz, "event journal and event are required");
		return -1;
	}
	if (event->phase != ME_EVENT_PHASE_START &&
	    event->phase != ME_EVENT_PHASE_UPDATE &&
	    event->phase != ME_EVENT_PHASE_END) {
		journal_set_err(err, errsz, "event journal received invalid phase");
		return -1;
	}
	g_mutex_lock(&journal->lock);
	if (journal->next_cursor == UINT64_MAX) {
		g_mutex_unlock(&journal->lock);
		journal_set_err(err, errsz, "event journal cursor exhausted");
		return -1;
	}
	next_cursor = journal->next_cursor + 1;
	line = record_to_line(next_cursor, event, &line_bytes);
	if (!line) {
		g_mutex_unlock(&journal->lock);
		journal_set_err(err, errsz, "serialize event journal record failed");
		return -1;
	}
	rc = compact_to_fit_locked(journal, line_bytes, event->phase, err, errsz);
	if (rc != 0) {
		g_free(line);
		g_mutex_unlock(&journal->lock);
		return rc;
	}
	if (fseek(journal->file, 0, SEEK_END) != 0 ||
	    (old_end = ftello(journal->file)) < 0 ||
	    fwrite(line, 1, (size_t)line_bytes, journal->file) !=
			(size_t)line_bytes || fflush(journal->file) != 0 ||
	    fsync(fileno(journal->file)) != 0) {
		int saved_errno = errno;
		if (old_end >= 0)
			ftruncate(fileno(journal->file), old_end);
		clearerr(journal->file);
		fseek(journal->file, 0, SEEK_END);
		g_free(line);
		g_mutex_unlock(&journal->lock);
		journal_set_err(err, errsz, "durable event journal append failed");
		errno = saved_errno;
		return -1;
	}
	record = g_new0(JournalRecord, 1);
	if (!record) {
		ftruncate(fileno(journal->file), old_end);
		fseek(journal->file, 0, SEEK_END);
		g_free(line);
		g_mutex_unlock(&journal->lock);
		journal_set_err(err, errsz, "event journal record allocation failed");
		return -1;
	}
	record->public_record.cursor = next_cursor;
	record->public_record.event = *event;
	record->bytes = line_bytes;
	g_queue_push_tail(journal->records, record);
	journal->bytes += line_bytes;
	journal->next_cursor = next_cursor;
	if (cursor)
		*cursor = next_cursor;
	g_free(line);
	g_mutex_unlock(&journal->lock);
	return 0;
}

GPtrArray *me_event_journal_snapshot_after(
		MeEventJournal *journal, uint64_t after_cursor, bool *replay_gap,
		uint64_t *oldest_cursor, uint64_t *latest_cursor)
{
	GPtrArray *snapshot;
	GList *it;
	uint64_t previous_cursor;

	if (replay_gap)
		*replay_gap = false;
	if (oldest_cursor)
		*oldest_cursor = 0;
	if (latest_cursor)
		*latest_cursor = 0;
	if (!journal)
		return NULL;
	snapshot = g_ptr_array_new_with_free_func(g_free);
	if (!snapshot)
		return NULL;
	g_mutex_lock(&journal->lock);
	if (!g_queue_is_empty(journal->records)) {
		JournalRecord *first = g_queue_peek_head(journal->records);
		JournalRecord *last = g_queue_peek_tail(journal->records);
		if (oldest_cursor)
			*oldest_cursor = first->public_record.cursor;
		if (latest_cursor)
			*latest_cursor = last->public_record.cursor;
	}
	previous_cursor = after_cursor;
	for (it = journal->records->head; it; it = it->next) {
		JournalRecord *record = it->data;
		MeEventJournalRecord *copy;
		if (record->public_record.cursor <= after_cursor)
			continue;
		if (replay_gap && record->public_record.cursor > previous_cursor &&
				record->public_record.cursor - previous_cursor > 1)
			*replay_gap = true;
		previous_cursor = record->public_record.cursor;
		copy = g_new(MeEventJournalRecord, 1);
		if (!copy) {
			g_mutex_unlock(&journal->lock);
			g_ptr_array_free(snapshot, TRUE);
			return NULL;
		}
		*copy = record->public_record;
		g_ptr_array_add(snapshot, copy);
	}
	g_mutex_unlock(&journal->lock);
	return snapshot;
}

uint64_t me_event_journal_latest_cursor(MeEventJournal *journal)
{
	uint64_t value = 0;
	if (!journal)
		return 0;
	g_mutex_lock(&journal->lock);
	value = journal->next_cursor;
	g_mutex_unlock(&journal->lock);
	return value;
}

uint64_t me_event_journal_dropped_updates(MeEventJournal *journal)
{
	uint64_t value = 0;
	if (!journal)
		return 0;
	g_mutex_lock(&journal->lock);
	value = journal->dropped_updates;
	g_mutex_unlock(&journal->lock);
	return value;
}
