#include "session/session_mgr.h"

#include "common/util.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>

struct SessionMgr {
	LiveBackend backend;
	GHashTable *sessions; /* session_id -> MediaSession* */
};

SessionMgr *session_mgr_new(const LiveBackend *backend)
{
	SessionMgr *m;

	if (!backend || !backend->start_live || !backend->stop_live)
		return NULL;
	m = g_new0(SessionMgr, 1);
	m->backend = *backend;
	m->sessions =
	    g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	return m;
}

void session_mgr_free(SessionMgr *m)
{
	GHashTableIter iter;
	GList *ids = NULL;
	GList *it;
	gpointer key, value;

	if (!m)
		return;

	g_hash_table_iter_init(&iter, m->sessions);
	while (g_hash_table_iter_next(&iter, &key, &value))
		ids = g_list_prepend(ids, g_strdup((const char *)key));

	for (it = ids; it; it = it->next) {
		char err[160];
		int rc = m->backend.stop_live(m->backend.data, (const char *)it->data,
		                              err, sizeof(err));
		if (rc != ME_ERR_OK)
			me_log(ME_LOG_WARN,
			       "session %s stop during shutdown failed: %s",
			       (const char *)it->data, err);
	}
	g_list_free_full(ids, g_free);

	g_hash_table_destroy(m->sessions);
	g_free(m);
}

bool session_mgr_has(SessionMgr *m, const char *session_id)
{
	return g_hash_table_contains(m->sessions, session_id);
}

int session_mgr_count(SessionMgr *m)
{
	return (int)g_hash_table_size(m->sessions);
}

const MediaSession *session_mgr_get(SessionMgr *m, const char *session_id)
{
	return g_hash_table_lookup(m->sessions, session_id);
}

const MediaSession *session_mgr_active(SessionMgr *m)
{
	GHashTableIter iter;
	gpointer key, value;

	g_hash_table_iter_init(&iter, m->sessions);
	if (g_hash_table_iter_next(&iter, &key, &value))
		return value;
	return NULL;
}

int session_mgr_start(SessionMgr *m, const SessionParams *p, char *err,
                      size_t errsz)
{
	MediaSession *s;
	int rc;

	if (session_mgr_has(m, p->session_id)) {
		me_set_err(err, errsz, "session already exists: %s", p->session_id);
		return ME_ERR_MEDIA;
	}
	if (g_hash_table_size(m->sessions) > 0) {
		me_set_err(err, errsz, "media busy: one live session already active");
		return ME_ERR_MEDIA;
	}

	rc = m->backend.start_live(m->backend.data, p, err, errsz);
	if (rc != ME_ERR_OK)
		return rc;

	s = g_new0(MediaSession, 1);
	s->params = *p;
	s->last_activity_us = g_get_monotonic_time();
	g_hash_table_insert(m->sessions, g_strdup(p->session_id), s);
	me_log(ME_LOG_INFO,
	       "session started: session_id=%s channel_id=%s target=%s:%d "
	       "codec=%s ssrc=%u pt=%d lease_started_us=%" G_GINT64_FORMAT,
	       p->session_id, p->channel_id, p->dest_ip, p->dest_port, p->codec,
	       p->ssrc, p->payload_type, s->last_activity_us);
	return ME_ERR_OK;
}

int session_mgr_touch(SessionMgr *m, const char *session_id, char *err,
                      size_t errsz)
{
	MediaSession *s = g_hash_table_lookup(m->sessions, session_id);

	if (!s) {
		me_set_err(err, errsz, "session not found: %s", session_id);
		return ME_ERR_NOT_FOUND;
	}
	s->last_activity_us = g_get_monotonic_time();
	return ME_ERR_OK;
}

bool session_mgr_expired(SessionMgr *m, int64_t now_us, int64_t timeout_us,
                         char *session_id, size_t session_id_sz)
{
	GHashTableIter iter;
	gpointer key, value;
	MediaSession *s;

	if (timeout_us <= 0)
		return false;
	g_hash_table_iter_init(&iter, m->sessions);
	if (!g_hash_table_iter_next(&iter, &key, &value))
		return false;
	s = value;
	if (now_us - s->last_activity_us < timeout_us)
		return false;
	if (session_id && session_id_sz > 0)
		snprintf(session_id, session_id_sz, "%s", (const char *)key);
	return true;
}

int session_mgr_stop(SessionMgr *m, const char *session_id, char *err,
                     size_t errsz)
{
	MediaSession *s = g_hash_table_lookup(m->sessions, session_id);
	char channel[ME_CHANNEL_ID_MAX];
	char dest_ip[ME_DEST_IP_MAX];
	uint32_t ssrc;
	int dest_port;
	int rc;

	if (!s) {
		me_log(ME_LOG_INFO, "session stop idempotent: session_id=%s not found",
		       session_id);
		return ME_ERR_OK;
	}

	rc = m->backend.stop_live(m->backend.data, session_id, err, errsz);
	if (rc != ME_ERR_OK && rc != ME_ERR_NOT_FOUND) {
		me_log(ME_LOG_WARN,
		       "session stop failed: session_id=%s rc=%d error=%s (retained for retry)",
		       session_id, rc, err && err[0] ? err : "unknown");
		return rc;
	}
	/* Copy fields before removing the hash entry.  The value destructor owns
	 * MediaSession, so retaining pointers into `s` after remove is a
	 * use-after-free in the stop log. */
	snprintf(channel, sizeof(channel), "%s", s->params.channel_id);
	snprintf(dest_ip, sizeof(dest_ip), "%s", s->params.dest_ip);
	dest_port = s->params.dest_port;
	ssrc = s->params.ssrc;
	g_hash_table_remove(m->sessions, session_id);
	me_log(ME_LOG_INFO,
	       "session stopped: session_id=%s channel_id=%s target=%s:%d "
	       "ssrc=%u result=%s",
	       session_id, channel, dest_ip, dest_port, ssrc,
	       rc == ME_ERR_NOT_FOUND ? "already-gone" : "ok");
	return ME_ERR_OK;
}
