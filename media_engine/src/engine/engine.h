#ifndef ME_ENGINE_H
#define ME_ENGINE_H

#include <stdbool.h>
#include <stddef.h>

#include "aiq/aiq_ctrl.h"
#include "common/me_errors.h"
#include "config/config.h"
#include "gst/gst_runner.h"
#include "session/session_mgr.h"

typedef struct {
	bool running;
	int fps;
	int bitrate;
} EngineStatus;

typedef void (*EngineEventCb)(void *userdata, const char *event,
                              const char *session_id, const char *message);

typedef struct {
	EngineConfig cfg;
	AiqCtrl aiq;
	GstRunner *runner;
	SessionMgr *sessions;
	EngineEventCb event_cb;
	void *event_userdata;
} Engine;

int engine_init(Engine *e, const EngineConfig *cfg, char *err, size_t errsz);
void engine_deinit(Engine *e);
/* Wires media-plane events (error/eos/state) to an external sink, e.g. the
 * IPC server's media.event broadcast. */
void engine_set_event_sink(Engine *e, EngineEventCb cb, void *userdata);

int engine_start_live(Engine *e, const SessionParams *p, char *err,
                      size_t errsz);
int engine_stop_live(Engine *e, const char *session_id, char *err,
                     size_t errsz);
int engine_snapshot(Engine *e, const char *channel_id, char *err,
                    size_t errsz);
int engine_get_status(Engine *e, EngineStatus *st);

#endif /* ME_ENGINE_H */
