#ifndef ME_SESSION_MGR_H
#define ME_SESSION_MGR_H

#include <stdbool.h>
#include <stddef.h>

#include "common/me_errors.h"
#include "session/session.h"

/* A live-stream session as seen by the rest of the engine. */
typedef struct {
	SessionParams params;
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

/* Starts one live session: rejects duplicate ids and concurrent sessions with
 * ME_ERR_MEDIA, delegates the actual pipeline work to the backend and only
 * registers the session after the backend succeeds. */
int session_mgr_start(SessionMgr *m, const SessionParams *p, char *err,
                      size_t errsz);
/* Stops and removes the session. ME_ERR_NOT_FOUND when absent. The session is
 * removed even when the backend stop fails so a BYE can never wedge the
 * engine; the failure is still returned to the caller. */
int session_mgr_stop(SessionMgr *m, const char *session_id, char *err,
                     size_t errsz);

#endif /* ME_SESSION_MGR_H */
