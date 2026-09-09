#include "analytics/evidence_cache.h"

#include "cJSON.h"

#include <glib.h>
#include <glib/gstdio.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define ME_EVIDENCE_METADATA_VERSION 2U
#define ME_EVIDENCE_FRAME_CACHE_MAX_BYTES (8ULL * 1024ULL * 1024ULL)
#define ME_EVIDENCE_NOTE_LIMIT 256U

typedef struct {
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	uint64_t stream_epoch;
	uint64_t frame_id;
	bool source_pts_valid;
	int64_t source_pts;
	MeTimebase source_timebase;
	uint32_t width;
	uint32_t height;
} FrameNote;

typedef struct {
	GBytes *jpeg;
	FrameNote note;
} JpegFrame;

struct MeEvidenceCache {
	GMutex lock;
	char directory[ME_EVIDENCE_PATH_MAX];
	uint64_t max_bytes;
	uint32_t retention_s;
	uint64_t frame_cache_max_bytes;
	uint64_t frame_cache_bytes;
	uint64_t stored_bytes;
	GHashTable *records;
	GQueue *notes;
	GQueue *frames;
};

static void set_err(char *err, size_t errsz, const char *fmt, ...)
{
	va_list ap;

	if (!err || errsz == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

static int64_t now_us(void)
{
	return g_get_real_time();
}

static bool valid_component(const char *value)
{
	const unsigned char *p;

	if (!value || !*value || !strcmp(value, ".") || !strcmp(value, ".."))
		return false;
	for (p = (const unsigned char *)value; *p; p++) {
		if (!(g_ascii_isalnum(*p) || *p == '-' || *p == '_' || *p == '.'))
			return false;
	}
	return true;
}

static bool valid_sha256(const char *value)
{
	const unsigned char *p;

	if (!value || strlen(value) != 64)
		return false;
	for (p = (const unsigned char *)value; *p; p++) {
		if (!g_ascii_isxdigit(*p))
			return false;
	}
	return true;
}

static bool same_pts(const FrameNote *left, bool valid, int64_t pts)
{
	return left && valid && left->source_pts_valid &&
		left->source_pts == pts;
}

static bool same_timebase(MeTimebase left, MeTimebase right)
{
	return left.num != 0 && left.den != 0 && left.num == right.num &&
		left.den == right.den;
}

static void free_note(gpointer data)
{
	g_free(data);
}

static void free_frame(gpointer data)
{
	JpegFrame *frame = data;

	if (!frame)
		return;
	g_bytes_unref(frame->jpeg);
	g_free(frame);
}

static void free_record(gpointer data)
{
	g_free(data);
}

static gchar *jpeg_path(const MeEvidenceCache *cache, const char *evidence_id)
{
	gchar *name;
	gchar *path;

	name = g_strdup_printf("%s.jpg", evidence_id);
	path = g_build_filename(cache->directory, name, NULL);
	g_free(name);
	return path;
}

static gchar *json_path(const MeEvidenceCache *cache, const char *evidence_id)
{
	gchar *name;
	gchar *path;

	name = g_strdup_printf("%s.json", evidence_id);
	path = g_build_filename(cache->directory, name, NULL);
	g_free(name);
	return path;
}

static int sync_parent_directory(const char *path)
{
	gchar *directory;
	int fd;
	int rc;

	directory = g_path_get_dirname(path);
	fd = open(directory, O_RDONLY | O_CLOEXEC);
	g_free(directory);
	if (fd < 0)
		return -1;
	rc = fsync(fd);
	close(fd);
	return rc;
}

static int sync_directory_path(const char *path)
{
	int fd;
	int rc;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	rc = fsync(fd);
	close(fd);
	return rc;
}

static int write_all(int fd, const unsigned char *data, size_t length)
{
	size_t offset = 0;

	while (offset < length) {
		ssize_t written = write(fd, data + offset, length - offset);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0)
			return -1;
		offset += (size_t)written;
	}
	return 0;
}

static int write_atomic(const char *path, const unsigned char *data,
				size_t length, char *err, size_t errsz)
{
	gchar *template;
	int fd = -1;
	int close_rc;
	int rc = -1;

	template = g_strdup_printf("%s.tmp-XXXXXX", path);
	fd = g_mkstemp(template);
	if (fd < 0) {
		set_err(err, errsz, "create temporary evidence file %s: %s", path,
				g_strerror(errno));
		g_free(template);
		return -1;
	}
	if (write_all(fd, data, length) != 0 || fsync(fd) != 0) {
		set_err(err, errsz, "write evidence file %s: %s", path,
				g_strerror(errno));
		goto out;
	}
	close_rc = close(fd);
	fd = -1;
	if (close_rc != 0) {
		set_err(err, errsz, "close evidence file %s: %s", path,
				g_strerror(errno));
		goto out;
	}
	if (rename(template, path) != 0) {
		set_err(err, errsz, "publish evidence file %s: %s", path,
				g_strerror(errno));
		goto out;
	}
	if (sync_parent_directory(path) != 0) {
		set_err(err, errsz, "sync evidence directory for %s: %s", path,
				g_strerror(errno));
		goto out;
	}
	rc = 0;
out:
	if (fd >= 0)
		close(fd);
	if (rc != 0)
		unlink(template);
	g_free(template);
	return rc;
}

static bool json_string(cJSON *object, const char *name, char *out,
				size_t outsz, bool required)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

	if (!item || !cJSON_IsString(item) || !item->valuestring ||
		(item->valuestring[0] == '\0' && required))
		return !required;
	if (out && outsz > 0)
		g_strlcpy(out, item->valuestring, outsz);
	return true;
}

static bool json_u64(cJSON *object, const char *name, uint64_t *out,
				 bool required)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

	if (!item || !cJSON_IsNumber(item) || item->valuedouble < 0.0)
		return !required;
	if (out)
		*out = (uint64_t)item->valuedouble;
	return true;
}

static bool json_i64(cJSON *object, const char *name, int64_t *out,
				 bool required)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

	if (!item || !cJSON_IsNumber(item))
		return !required;
	if (out)
		*out = (int64_t)item->valuedouble;
	return true;
}

static bool json_bool(cJSON *object, const char *name, bool *out,
				  bool required)
{
	cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);

	if (!item || !cJSON_IsBool(item))
		return !required;
	if (out)
		*out = cJSON_IsTrue(item);
	return true;
}

static cJSON *record_to_json(const MeEvidenceRecord *record)
{
	cJSON *object;
	gchar *object_key;

	object = cJSON_CreateObject();
	if (!object)
		return NULL;
	cJSON_AddNumberToObject(object, "v", ME_EVIDENCE_METADATA_VERSION);
	cJSON_AddStringToObject(object, "evidence_id", record->evidence_id);
	cJSON_AddStringToObject(object, "event_id", record->event_id);
	cJSON_AddStringToObject(object, "channel_id", record->channel_id);
	cJSON_AddNumberToObject(object, "stream_epoch",
					(double)record->stream_epoch);
	cJSON_AddNumberToObject(object, "frame_id", (double)record->frame_id);
	cJSON_AddBoolToObject(object, "source_pts_valid", record->source_pts_valid);
	cJSON_AddNumberToObject(object, "source_pts", (double)record->source_pts);
	cJSON_AddNumberToObject(object, "source_timebase_num",
					(double)record->source_timebase.num);
	cJSON_AddNumberToObject(object, "source_timebase_den",
					(double)record->source_timebase.den);
	cJSON_AddNumberToObject(object, "capture_time_us",
					(double)record->capture_time_us);
	cJSON_AddNumberToObject(object, "width", record->width);
	cJSON_AddNumberToObject(object, "height", record->height);
	cJSON_AddNumberToObject(object, "bytes", (double)record->bytes);
	cJSON_AddStringToObject(object, "sha256", record->sha256);
	cJSON_AddStringToObject(object, "accuracy",
					me_evidence_accuracy_name(record->accuracy));
	cJSON_AddNumberToObject(object, "frame_delta",
					(double)record->frame_delta);
	cJSON_AddNumberToObject(object, "pts_delta",
					(double)record->pts_delta);
	cJSON_AddNumberToObject(object, "created_at_us",
					(double)record->created_at_us);
	cJSON_AddNumberToObject(object, "expires_at_us",
					(double)record->expires_at_us);
	cJSON_AddStringToObject(object, "storage_state", record->storage_state);
	cJSON_AddStringToObject(object, "delivery_state", record->delivery_state);
	object_key = g_strdup_printf("%s.jpg", record->evidence_id);
	if (!object_key) {
		cJSON_Delete(object);
		return NULL;
	}
	cJSON_AddStringToObject(object, "object_key", object_key);
	g_free(object_key);
	return object;
}

static bool record_from_json(cJSON *object, MeEvidenceRecord *record)
{
	cJSON *version;
	cJSON *accuracy;
	uint64_t width;
	uint64_t height;
	uint64_t source_timebase_num;
	uint64_t source_timebase_den;
	char object_key[ME_EVIDENCE_PATH_MAX];
	gchar *expected_object_key;

	memset(record, 0, sizeof(*record));
	version = cJSON_GetObjectItemCaseSensitive(object, "v");
	if (!version || !cJSON_IsNumber(version) ||
		(unsigned)version->valuedouble != ME_EVIDENCE_METADATA_VERSION)
		return false;
	if (!json_string(object, "evidence_id", record->evidence_id,
				 sizeof(record->evidence_id), true) ||
		!valid_component(record->evidence_id) ||
		!json_string(object, "event_id", record->event_id,
				 sizeof(record->event_id), true) ||
		!json_string(object, "channel_id", record->channel_id,
				 sizeof(record->channel_id), true) ||
		!json_u64(object, "stream_epoch", &record->stream_epoch, true) ||
		!json_u64(object, "frame_id", &record->frame_id, true) ||
		!json_bool(object, "source_pts_valid", &record->source_pts_valid, true) ||
		!json_i64(object, "source_pts", &record->source_pts, true) ||
		!json_u64(object, "source_timebase_num", &source_timebase_num, true) ||
		!json_u64(object, "source_timebase_den", &source_timebase_den, true) ||
		!json_i64(object, "capture_time_us", &record->capture_time_us, true) ||
		!json_u64(object, "width", &width, true) ||
		!json_u64(object, "height", &height, true) ||
		!json_u64(object, "bytes", &record->bytes, true) ||
		!json_string(object, "sha256", record->sha256,
				 sizeof(record->sha256), true) ||
		!valid_sha256(record->sha256) ||
		!json_u64(object, "frame_delta", &record->frame_delta, true) ||
		!json_i64(object, "pts_delta", &record->pts_delta, true) ||
		!json_i64(object, "created_at_us", &record->created_at_us, true) ||
		!json_i64(object, "expires_at_us", &record->expires_at_us, true) ||
		!json_string(object, "storage_state", record->storage_state,
				 sizeof(record->storage_state), true) ||
		!json_string(object, "delivery_state", record->delivery_state,
				 sizeof(record->delivery_state), true) ||
		!json_string(object, "object_key", object_key, sizeof(object_key), true))
		return false;
	expected_object_key = g_strdup_printf("%s.jpg", record->evidence_id);
	if (!expected_object_key || strcmp(object_key, expected_object_key) != 0) {
		g_free(expected_object_key);
		return false;
	}
	g_free(expected_object_key);
	if (width > UINT32_MAX || height > UINT32_MAX ||
		source_timebase_num > UINT32_MAX || source_timebase_den > UINT32_MAX)
		return false;
	record->width = (uint32_t)width;
	record->height = (uint32_t)height;
	record->source_timebase.num = (uint32_t)source_timebase_num;
	record->source_timebase.den = (uint32_t)source_timebase_den;
	if (record->source_pts_valid &&
		(record->source_timebase.num == 0 || record->source_timebase.den == 0))
		return false;
	accuracy = cJSON_GetObjectItemCaseSensitive(object, "accuracy");
	if (!accuracy || !cJSON_IsString(accuracy) || !accuracy->valuestring)
		return false;
	if (!strcmp(accuracy->valuestring, "exact"))
		record->accuracy = ME_EVIDENCE_ACCURACY_EXACT;
	else if (!strcmp(accuracy->valuestring, "approximate"))
		record->accuracy = ME_EVIDENCE_ACCURACY_APPROXIMATE;
	else
		return false;
	return true;
}

static bool make_evidence_id(const MeAnalyticsEvent *event, char *out,
				 size_t outsz)
{
	gchar *digest;
	int written;

	if (!event || !out || outsz == 0 || !event->event_id[0])
		return false;
	digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, event->event_id,
								-1);
	if (!digest)
		return false;
	written = g_snprintf(out, outsz, "ev-%s", digest);
	g_free(digest);
	return written > 0 && (size_t)written < outsz && valid_component(out);
}

static void fill_record_path(const MeEvidenceCache *cache,
					 MeEvidenceRecord *record)
{
	gchar *path = jpeg_path(cache, record->evidence_id);

	g_strlcpy(record->path, path, sizeof(record->path));
	g_free(path);
}

static FrameNote *latest_note_for_pts_locked(MeEvidenceCache *cache,
							 bool valid, int64_t pts)
{
	GList *it;

	for (it = g_queue_peek_tail_link(cache->notes); it; it = it->prev) {
		FrameNote *note = it->data;

		if (same_pts(note, valid, pts))
			return note;
	}
	return NULL;
}

static void apply_note_to_frame(JpegFrame *frame, const FrameNote *note)
{
	if (!frame || !note)
		return;
	frame->note = *note;
}

static bool frame_matches_event(const JpegFrame *frame,
					const MeAnalyticsEvent *event)
{
	if (!frame || !event)
		return false;
	if (!frame->note.channel_id[0] || frame->note.stream_epoch == 0)
		return false;
	if (strcmp(frame->note.channel_id, event->channel_id) != 0)
		return false;
	if (frame->note.stream_epoch != event->stream_epoch)
		return false;
	return true;
}

static bool frame_is_exact(const JpegFrame *frame, const MeAnalyticsEvent *event)
{
	if (!frame_matches_event(frame, event))
		return false;
	if (event->source_pts_valid && frame->note.source_pts_valid &&
		same_timebase(frame->note.source_timebase, event->source_timebase))
		return frame->note.source_pts == event->source_pts;
	return event->frame_id > 0 && frame->note.frame_id == event->frame_id;
}

static int64_t abs_i64_delta(int64_t left, int64_t right)
{
	if (left >= right)
		return left - right;
	return right - left;
}

static JpegFrame *select_frame_locked(MeEvidenceCache *cache,
						const MeAnalyticsEvent *event,
						MeEvidenceAccuracy *accuracy,
						uint64_t *frame_delta,
						int64_t *pts_delta)
{
	GList *it;
	JpegFrame *nearest = NULL;
	int64_t nearest_delta = INT64_MAX;

	*accuracy = ME_EVIDENCE_ACCURACY_NONE;
	*frame_delta = 0;
	*pts_delta = 0;
	for (it = g_queue_peek_tail_link(cache->frames); it; it = it->prev) {
		JpegFrame *frame = it->data;

		if (frame_is_exact(frame, event)) {
			*accuracy = ME_EVIDENCE_ACCURACY_EXACT;
			return frame;
		}
		if (!frame_matches_event(frame, event))
			continue;
		if (event->source_pts_valid && frame->note.source_pts_valid &&
			same_timebase(frame->note.source_timebase,
					  event->source_timebase)) {
			int64_t delta = abs_i64_delta(frame->note.source_pts,
								 event->source_pts);
			if (delta < nearest_delta) {
				nearest_delta = delta;
				nearest = frame;
			}
		} else if ((!event->source_pts_valid || !frame->note.source_pts_valid) &&
			   !nearest) {
			nearest = frame;
		}
	}
	if (!nearest)
		return NULL;
	*accuracy = ME_EVIDENCE_ACCURACY_APPROXIMATE;
	if (event->frame_id > 0 && nearest->note.frame_id > 0) {
		*frame_delta = nearest->note.frame_id > event->frame_id
			? nearest->note.frame_id - event->frame_id
			: event->frame_id - nearest->note.frame_id;
	}
	if (event->source_pts_valid && nearest->note.source_pts_valid &&
		same_timebase(nearest->note.source_timebase, event->source_timebase))
		*pts_delta = nearest->note.source_pts - event->source_pts;
	return nearest;
}

static void remove_record_locked(MeEvidenceCache *cache, const char *id)
{
	MeEvidenceRecord *record;
	gchar *metadata;

	record = g_hash_table_lookup(cache->records, id);
	if (!record)
		return;
	metadata = json_path(cache, id);
	unlink(record->path);
	unlink(metadata);
	g_free(metadata);
	if (cache->stored_bytes >= record->bytes)
		cache->stored_bytes -= record->bytes;
	else
		cache->stored_bytes = 0;
	g_hash_table_remove(cache->records, id);
}

static uint64_t prune_expired_locked(MeEvidenceCache *cache, int64_t now)
{
	GPtrArray *expired = g_ptr_array_new_with_free_func(g_free);
	GHashTableIter iter;
	gpointer key;
	gpointer value;
	uint64_t removed = 0;
	unsigned i;

	g_hash_table_iter_init(&iter, cache->records);
	while (g_hash_table_iter_next(&iter, &key, &value)) {
		MeEvidenceRecord *record = value;

		if (record->expires_at_us > 0 && record->expires_at_us <= now)
			g_ptr_array_add(expired, g_strdup((const char *)key));
	}
	for (i = 0; i < expired->len; i++) {
		remove_record_locked(cache, g_ptr_array_index(expired, i));
		removed++;
	}
	g_ptr_array_free(expired, TRUE);
	return removed;
}

static int load_records(MeEvidenceCache *cache, char *err, size_t errsz)
{
	GDir *dir;
	const gchar *name;

	dir = g_dir_open(cache->directory, 0, NULL);
	if (!dir)
		return 0;
	while ((name = g_dir_read_name(dir)) != NULL) {
		const char *suffix;
		gchar *path;
		gchar *contents = NULL;
		gsize length = 0;
		cJSON *object;
		MeEvidenceRecord *record;
		struct stat st;

		suffix = g_strrstr(name, ".json");
		if (!suffix || suffix[5] != '\0')
			continue;
		{
			gchar *id = g_strndup(name, suffix - name);

			if (!valid_component(id)) {
				g_free(id);
				continue;
			}
			path = g_build_filename(cache->directory, name, NULL);
			if (!g_file_get_contents(path, &contents, &length, NULL)) {
				g_free(path);
				g_free(id);
				continue;
			}
			object = cJSON_ParseWithLength(contents, length);
			record = g_new0(MeEvidenceRecord, 1);
			if (!object || !record_from_json(object, record) ||
				strcmp(record->evidence_id, id) != 0) {
				set_err(err, errsz, "invalid evidence metadata %s", path);
				if (object)
					cJSON_Delete(object);
				g_free(record);
				g_free(contents);
				unlink(path);
				g_free(path);
				g_free(id);
				continue;
			}
			cJSON_Delete(object);
			g_free(contents);
			fill_record_path(cache, record);
			if (stat(record->path, &st) != 0 || !S_ISREG(st.st_mode) ||
				(uint64_t)st.st_size != record->bytes) {
				set_err(err, errsz, "evidence JPEG missing or truncated %s",
					record->path);
				unlink(path);
				unlink(record->path);
				g_free(record);
				g_free(path);
				g_free(id);
				continue;
			}
			{
				gchar *jpeg_contents = NULL;
				gsize jpeg_length = 0;
				gchar *digest;

				if (!g_file_get_contents(record->path, &jpeg_contents,
						&jpeg_length, NULL) || jpeg_length != record->bytes) {
					set_err(err, errsz, "cannot read evidence JPEG %s",
							record->path);
					g_free(jpeg_contents);
					unlink(path);
					unlink(record->path);
					g_free(record);
					g_free(path);
					g_free(id);
					continue;
				}
				digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256,
						(const guchar *)jpeg_contents, jpeg_length);
				if (!digest || g_ascii_strcasecmp(digest, record->sha256) != 0) {
					set_err(err, errsz, "evidence JPEG checksum mismatch %s",
							record->path);
					g_free(digest);
					g_free(jpeg_contents);
					unlink(path);
					unlink(record->path);
					g_free(record);
					g_free(path);
					g_free(id);
					continue;
				}
				g_free(digest);
				g_free(jpeg_contents);
			}
			cache->stored_bytes += record->bytes;
			g_hash_table_insert(cache->records, g_strdup(id), record);
			g_free(path);
			g_free(id);
		}
	}
	g_dir_close(dir);
	return 0;
}

static bool evidence_id_from_suffix(const char *name, const char *suffix,
						char *out, size_t outsz)
{
	size_t name_len;
	size_t suffix_len;
	size_t id_len;

	if (!name || !suffix || !out || outsz == 0)
		return false;
	name_len = strlen(name);
	suffix_len = strlen(suffix);
	if (name_len <= suffix_len ||
		strcmp(name + name_len - suffix_len, suffix) != 0)
		return false;
	id_len = name_len - suffix_len;
	if (id_len + 1 > outsz)
		return false;
	memcpy(out, name, id_len);
	out[id_len] = '\0';
	return valid_component(out);
}

static bool is_evidence_temp_file(const char *name)
{
	const char *marker;
	char id[ME_ANALYTICS_EVIDENCE_ID_MAX];
	size_t id_len;

	if (!name)
		return false;
	marker = strstr(name, ".jpg.tmp-");
	if (!marker)
		marker = strstr(name, ".json.tmp-");
	if (!marker || marker == name || !marker[9])
		return false;
	id_len = (size_t)(marker - name);
	if (id_len + 1 > sizeof(id))
		return false;
	memcpy(id, name, id_len);
	id[id_len] = '\0';
	return valid_component(id);
}

static int cleanup_orphan_files(MeEvidenceCache *cache, char *err,
						size_t errsz)
{
	GDir *dir;
	const gchar *name;
	bool changed = false;

	dir = g_dir_open(cache->directory, 0, NULL);
	if (!dir)
		return 0;
	while ((name = g_dir_read_name(dir)) != NULL) {
		gchar *path;
		char evidence_id[ME_ANALYTICS_EVIDENCE_ID_MAX];
		bool remove_file = false;

		if (is_evidence_temp_file(name)) {
			remove_file = true;
		} else if (evidence_id_from_suffix(name, ".jpg", evidence_id,
							   sizeof(evidence_id))) {
			remove_file = g_hash_table_lookup(cache->records, evidence_id) == NULL;
		} else if (evidence_id_from_suffix(name, ".json", evidence_id,
							   sizeof(evidence_id))) {
			remove_file = g_hash_table_lookup(cache->records, evidence_id) == NULL;
		}
		if (!remove_file)
			continue;
		path = g_build_filename(cache->directory, name, NULL);
		if (unlink(path) != 0 && errno != ENOENT) {
			set_err(err, errsz, "remove orphan evidence file %s: %s", path,
					g_strerror(errno));
			g_free(path);
			g_dir_close(dir);
			return -1;
		}
		changed = true;
		g_free(path);
	}
	g_dir_close(dir);
	if (changed && sync_directory_path(cache->directory) != 0) {
		set_err(err, errsz, "sync evidence directory after orphan cleanup: %s",
				g_strerror(errno));
		return -1;
	}
	return 0;
}

static int copy_record(MeEvidenceRecord *out, const MeEvidenceRecord *record)
{
	if (!out || !record)
		return -1;
	*out = *record;
	return 0;
}

const char *me_evidence_accuracy_name(MeEvidenceAccuracy accuracy)
{
	switch (accuracy) {
	case ME_EVIDENCE_ACCURACY_EXACT:
		return "exact";
	case ME_EVIDENCE_ACCURACY_APPROXIMATE:
		return "approximate";
	default:
		return "none";
	}
}

MeEvidenceCache *me_evidence_cache_open(const char *directory,
						uint64_t max_bytes,
						uint32_t retention_s, char *err,
						size_t errsz)
{
	MeEvidenceCache *cache;
	int mkdir_rc;

	if (!directory || !*directory || max_bytes == 0 || retention_s == 0) {
		set_err(err, errsz, "evidence cache directory and limits are required");
		return NULL;
	}
	if (strlen(directory) >= ME_EVIDENCE_PATH_MAX) {
		set_err(err, errsz, "evidence cache directory is too long");
		return NULL;
	}
	mkdir_rc = g_mkdir_with_parents(directory, 0700);
	if (mkdir_rc != 0) {
		set_err(err, errsz, "create evidence cache directory %s: %s", directory,
				g_strerror(errno));
		return NULL;
	}
	cache = g_new0(MeEvidenceCache, 1);
	g_mutex_init(&cache->lock);
	g_strlcpy(cache->directory, directory, sizeof(cache->directory));
	cache->max_bytes = max_bytes;
	cache->retention_s = retention_s;
	cache->frame_cache_max_bytes = MIN(max_bytes,
								ME_EVIDENCE_FRAME_CACHE_MAX_BYTES);
	cache->records = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
									  free_record);
	cache->notes = g_queue_new();
	cache->frames = g_queue_new();
	if (!cache->records || !cache->notes || !cache->frames) {
		set_err(err, errsz, "allocate evidence cache state failed");
		me_evidence_cache_close(cache);
		return NULL;
	}
	load_records(cache, err, errsz);
	if (cleanup_orphan_files(cache, err, errsz) != 0) {
		me_evidence_cache_close(cache);
		return NULL;
	}
	g_mutex_lock(&cache->lock);
	prune_expired_locked(cache, now_us());
	g_mutex_unlock(&cache->lock);
	return cache;
}

void me_evidence_cache_close(MeEvidenceCache *cache)
{
	if (!cache)
		return;
	g_mutex_lock(&cache->lock);
	if (cache->frames)
		g_queue_free_full(cache->frames, free_frame);
	if (cache->notes)
		g_queue_free_full(cache->notes, free_note);
	if (cache->records)
		g_hash_table_destroy(cache->records);
	cache->frames = NULL;
	cache->notes = NULL;
	cache->records = NULL;
	g_mutex_unlock(&cache->lock);
	g_mutex_clear(&cache->lock);
	g_free(cache);
}

int me_evidence_cache_note_frame(MeEvidenceCache *cache,
					 const char *channel_id, uint64_t stream_epoch,
					 uint64_t frame_id, bool source_pts_valid,
					 int64_t source_pts, MeTimebase source_timebase,
					 uint32_t width,
					 uint32_t height)
{
	FrameNote *note;
	GList *it;
	unsigned count;

	if (!cache || !channel_id || !*channel_id || stream_epoch == 0 ||
		frame_id == 0)
		return -1;
	note = g_new0(FrameNote, 1);
	g_strlcpy(note->channel_id, channel_id, sizeof(note->channel_id));
	note->stream_epoch = stream_epoch;
	note->frame_id = frame_id;
	note->source_pts_valid = source_pts_valid;
	note->source_pts = source_pts;
	note->source_timebase = source_timebase;
	note->width = width;
	note->height = height;
	g_mutex_lock(&cache->lock);
	g_queue_push_tail(cache->notes, note);
	count = g_queue_get_length(cache->notes);
	while (count > ME_EVIDENCE_NOTE_LIMIT) {
		g_free(g_queue_pop_head(cache->notes));
		count--;
	}
	for (it = g_queue_peek_head_link(cache->frames); it; it = it->next) {
		JpegFrame *frame = it->data;

		if (!frame->note.channel_id[0] &&
			same_pts(note, frame->note.source_pts_valid,
					 frame->note.source_pts))
			apply_note_to_frame(frame, note);
	}
	g_mutex_unlock(&cache->lock);
	return 0;
}

int me_evidence_cache_push_jpeg(MeEvidenceCache *cache,
					const uint8_t *jpeg, size_t jpeg_size,
					bool source_pts_valid, int64_t source_pts)
{
	JpegFrame *frame;
	FrameNote *note;

	if (!cache || !jpeg || jpeg_size < 4 || jpeg[0] != 0xff ||
		jpeg[1] != 0xd8 || jpeg[jpeg_size - 2] != 0xff ||
		jpeg[jpeg_size - 1] != 0xd9)
		return -1;
	frame = g_new0(JpegFrame, 1);
	frame->jpeg = g_bytes_new(jpeg, jpeg_size);
	frame->note.source_pts_valid = source_pts_valid;
	frame->note.source_pts = source_pts;
	g_mutex_lock(&cache->lock);
	note = latest_note_for_pts_locked(cache, source_pts_valid, source_pts);
	if (note)
		apply_note_to_frame(frame, note);
	g_queue_push_tail(cache->frames, frame);
	cache->frame_cache_bytes += jpeg_size;
	while (cache->frame_cache_bytes > cache->frame_cache_max_bytes &&
		!g_queue_is_empty(cache->frames)) {
		JpegFrame *old = g_queue_pop_head(cache->frames);
		gsize old_size = g_bytes_get_size(old->jpeg);

		if (cache->frame_cache_bytes >= old_size)
			cache->frame_cache_bytes -= old_size;
		else
			cache->frame_cache_bytes = 0;
		free_frame(old);
	}
	g_mutex_unlock(&cache->lock);
	return 0;
}

int me_evidence_cache_capture(MeEvidenceCache *cache, MeAnalyticsEvent *event,
					  MeEvidenceRecord *record, char *err,
					  size_t errsz)
{
	JpegFrame *frame;
	MeEvidenceRecord candidate;
	MeEvidenceAccuracy accuracy;
	uint64_t frame_delta;
	int64_t pts_delta;
	GBytes *jpeg;
	const guint8 *jpeg_data;
	gsize jpeg_size;
	cJSON *metadata;
	gchar *metadata_text;
	gchar *jpeg_file;
	gchar *json_file;
	gchar *digest;
	int64_t now;

	if (!cache || !event || !event->event_id[0] || !event->channel_id[0] ||
		event->stream_epoch == 0) {
		set_err(err, errsz, "event is missing evidence identity");
		return -1;
	}
	if (!event->evidence_id[0] &&
		!make_evidence_id(event, event->evidence_id,
					 sizeof(event->evidence_id))) {
		set_err(err, errsz, "cannot derive evidence id for event %s",
				event->event_id);
		return -1;
	}
	if (!valid_component(event->evidence_id)) {
		set_err(err, errsz, "evidence id contains unsafe path characters");
		return -1;
	}
	g_mutex_lock(&cache->lock);
	if (g_hash_table_lookup(cache->records, event->evidence_id)) {
		if (record)
			copy_record(record,
					g_hash_table_lookup(cache->records, event->evidence_id));
		g_mutex_unlock(&cache->lock);
		return 0;
	}
	if (event->phase != ME_EVENT_PHASE_START) {
		g_mutex_unlock(&cache->lock);
		set_err(err, errsz, "evidence for event %s has not been published",
				event->event_id);
		return 1;
	}
	frame = select_frame_locked(cache, event, &accuracy, &frame_delta,
							&pts_delta);
	if (!frame || accuracy == ME_EVIDENCE_ACCURACY_NONE) {
		g_mutex_unlock(&cache->lock);
		set_err(err, errsz, "no recent JPEG matches event %s", event->event_id);
		return 1;
	}
	jpeg = g_bytes_ref(frame->jpeg);
	candidate.accuracy = accuracy;
	candidate.frame_delta = frame_delta;
	candidate.pts_delta = pts_delta;
	candidate.source_pts_valid = frame->note.source_pts_valid;
	candidate.source_pts = frame->note.source_pts;
	candidate.source_timebase = frame->note.source_timebase;
	candidate.frame_id = frame->note.frame_id > 0 ? frame->note.frame_id
									 : (accuracy == ME_EVIDENCE_ACCURACY_EXACT
										? event->frame_id : 0);
	candidate.stream_epoch = event->stream_epoch;
	candidate.width = frame->note.width;
	candidate.height = frame->note.height;
	g_strlcpy(candidate.evidence_id, event->evidence_id,
				 sizeof(candidate.evidence_id));
	g_strlcpy(candidate.event_id, event->event_id, sizeof(candidate.event_id));
	g_strlcpy(candidate.channel_id, event->channel_id,
				 sizeof(candidate.channel_id));
	now = now_us();
	candidate.capture_time_us = event->event_time_us;
	candidate.created_at_us = now;
	g_strlcpy(candidate.storage_state, "ready",
			 sizeof(candidate.storage_state));
	g_strlcpy(candidate.delivery_state, "awaiting_consumer",
			 sizeof(candidate.delivery_state));
	if ((uint64_t)cache->retention_s >
			(uint64_t)(INT64_MAX - now) / 1000000ULL)
		candidate.expires_at_us = INT64_MAX;
	else
		candidate.expires_at_us = now + (int64_t)cache->retention_s * 1000000;
	jpeg_data = g_bytes_get_data(jpeg, &jpeg_size);
	candidate.bytes = jpeg_size;
	digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, jpeg_data,
								jpeg_size);
	if (!digest) {
		g_bytes_unref(jpeg);
		g_mutex_unlock(&cache->lock);
		set_err(err, errsz, "compute evidence checksum failed");
		return -1;
	}
	g_strlcpy(candidate.sha256, digest, sizeof(candidate.sha256));
	g_free(digest);
	fill_record_path(cache, &candidate);
	if (candidate.bytes > cache->max_bytes) {
		g_bytes_unref(jpeg);
		g_mutex_unlock(&cache->lock);
		set_err(err, errsz, "evidence JPEG exceeds cache capacity");
		return 1;
	}
	prune_expired_locked(cache, now);
	if (cache->stored_bytes + candidate.bytes > cache->max_bytes) {
		g_bytes_unref(jpeg);
		g_mutex_unlock(&cache->lock);
		set_err(err, errsz, "evidence cache is full");
		return 1;
	}
	jpeg_file = jpeg_path(cache, candidate.evidence_id);
	json_file = json_path(cache, candidate.evidence_id);
	if (write_atomic(jpeg_file, jpeg_data, jpeg_size, err, errsz) != 0)
		goto publish_failed;
	metadata = record_to_json(&candidate);
	metadata_text = metadata ? cJSON_PrintUnformatted(metadata) : NULL;
	if (!metadata || !metadata_text) {
		set_err(err, errsz, "serialize evidence metadata failed");
		if (metadata)
			cJSON_Delete(metadata);
		g_free(metadata_text);
		unlink(jpeg_file);
		goto publish_failed;
	}
	cJSON_Delete(metadata);
	if (write_atomic(json_file, (const unsigned char *)metadata_text,
				strlen(metadata_text), err, errsz) != 0) {
		g_free(metadata_text);
		unlink(jpeg_file);
		unlink(json_file);
		goto publish_failed;
	}
	g_free(metadata_text);
	candidate.bytes = jpeg_size;
	{
		MeEvidenceRecord *stored = g_new(MeEvidenceRecord, 1);
		if (!stored) {
			set_err(err, errsz, "allocate evidence metadata failed");
			unlink(jpeg_file);
			unlink(json_file);
			g_free(jpeg_file);
			g_free(json_file);
			g_bytes_unref(jpeg);
			g_mutex_unlock(&cache->lock);
			return -1;
		}
		*stored = candidate;
		g_hash_table_insert(cache->records, g_strdup(candidate.evidence_id),
						stored);
	}
	cache->stored_bytes += candidate.bytes;
	if (record)
		*record = candidate;
	g_free(jpeg_file);
	g_free(json_file);
	g_bytes_unref(jpeg);
	g_mutex_unlock(&cache->lock);
	return 0;

publish_failed:
	unlink(jpeg_file);
	unlink(json_file);
	g_free(jpeg_file);
	g_free(json_file);
	g_bytes_unref(jpeg);
	g_mutex_unlock(&cache->lock);
	return -1;
}

int me_evidence_cache_lookup(MeEvidenceCache *cache, const char *evidence_id,
					 MeEvidenceRecord *record)
{
	MeEvidenceRecord *found;

	if (!cache || !evidence_id || !record)
		return -1;
	g_mutex_lock(&cache->lock);
	found = g_hash_table_lookup(cache->records, evidence_id);
	if (found)
		*record = *found;
	g_mutex_unlock(&cache->lock);
	return found ? 0 : 1;
}

uint64_t me_evidence_cache_bytes(MeEvidenceCache *cache)
{
	uint64_t value;

	if (!cache)
		return 0;
	g_mutex_lock(&cache->lock);
	value = cache->stored_bytes;
	g_mutex_unlock(&cache->lock);
	return value;
}

uint64_t me_evidence_cache_count(MeEvidenceCache *cache)
{
	uint64_t value;

	if (!cache)
		return 0;
	g_mutex_lock(&cache->lock);
	value = g_hash_table_size(cache->records);
	g_mutex_unlock(&cache->lock);
	return value;
}

uint64_t me_evidence_cache_prune(MeEvidenceCache *cache)
{
	uint64_t removed;

	if (!cache)
		return 0;
	g_mutex_lock(&cache->lock);
	removed = prune_expired_locked(cache, now_us());
	g_mutex_unlock(&cache->lock);
	return removed;
}
