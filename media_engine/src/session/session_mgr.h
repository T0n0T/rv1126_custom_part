#ifndef ME_SESSION_MGR_H
#define ME_SESSION_MGR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common/me_errors.h"
#include "session/session.h"

/* A live-stream session as seen by the rest of the engine. */
typedef struct {
	SessionParams params;
	/* Monotonic timestamp refreshed by media.keepalive_live. */
	int64_t last_activity_us;
} MediaSession;

/* Backend interface used by session_mgr to drive the media plane. gst_runner
 * implements it in production; tests can substitute a fake. This keeps
 * session_mgr independent of GStreamer internals. */
typedef struct LiveBackend {
	void *data;
	int (*start_live)(void *data, const SessionParams *p, char *err,
	                  size_t errsz);
	int (*stop_live)(void *data, const char *session_id, char *err,
	                 size_t errsz);
} LiveBackend;

typedef struct SessionMgr SessionMgr;

/* Creates an empty session manager bound to backend. V1 supports one live
 * session at a time. Returns NULL on allocation failure or a NULL backend. */
SessionMgr *session_mgr_new(const LiveBackend *backend);
/* Stops every active session through the backend, then frees the manager. */
void session_mgr_free(SessionMgr *m);

bool session_mgr_has(SessionMgr *m, const char *session_id);
int session_mgr_count(SessionMgr *m);
const MediaSession *session_mgr_get(SessionMgr *m, const char *session_id);
/* Returns the single active session, or NULL when idle. */
const MediaSession *session_mgr_active(SessionMgr *m);
/* Refreshes the lease for an active session. Idempotent stop/keepalive calls
 * are intentionally safe across delayed BYE and daemon reconnects. */
int session_mgr_touch(SessionMgr *m, const char *session_id, char *err,
                      size_t errsz);
bool session_mgr_expired(SessionMgr *m, int64_t now_us, int64_t timeout_us,
                         char *session_id, size_t session_id_sz);

/* Starts one live session: rejects duplicate ids and concurrent sessions with
 * ME_ERR_MEDIA, delegates the actual pipeline work to the backend and only
 * registers the session after the backend succeeds. */
int session_mgr_start(SessionMgr *m, const SessionParams *p, char *err,
                      size_t errsz);
/* Stops and removes the session. Stopping an absent session is idempotent and
 * returns ME_ERR_OK. A backend failure keeps the registry entry so retrying
 * stop can release the real branch rather than hiding a busy pipeline. */
int session_mgr_stop(SessionMgr *m, const char *session_id, char *err,
                     size_t errsz);

#endif /* ME_SESSION_MGR_H */
