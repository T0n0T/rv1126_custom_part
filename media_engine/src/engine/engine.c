#include "engine/engine.h"

#include "common/util.h"

#include <string.h>

static int engine_backend_start(void *data, const SessionParams *p, char *err,
                                size_t errsz)
{
	return gst_runner_start_live((GstRunner *)data, p, err, errsz);
}

static int engine_backend_stop(void *data, const char *session_id, char *err,
                               size_t errsz)
{
	return gst_runner_stop_live((GstRunner *)data, session_id, err, errsz);
}

static void engine_gst_event(void *userdata, const char *event,
                             const char *session_id, const char *message)
{
	Engine *e = userdata;

	if (e->event_cb)
		e->event_cb(e->event_userdata, event, session_id, message);
}

int engine_init(Engine *e, const EngineConfig *cfg, char *err, size_t errsz)
{
	LiveBackend backend;

	memset(e, 0, sizeof(*e));
	e->cfg = *cfg;

	/* AIQ first: the sensor/ISP must be up before the capture pipeline. A
	 * failure is not fatal to the control plane; start_live reports it. */
	if (aiq_ctrl_start(&e->aiq, cfg->cam_id, cfg->iq_dir, cfg->af_mode, err,
	                   errsz) != 0)
		me_log(ME_LOG_ERROR, "aiq start failed (IPC stays usable)");

	e->runner = gst_runner_new(cfg, err, errsz);
	if (!e->runner) {
		me_set_err(err, errsz, "gst_runner allocation failed");
		return -1;
	}
	gst_runner_set_event_cb(e->runner, engine_gst_event, e);

	backend.data = e->runner;
	backend.start_live = engine_backend_start;
	backend.stop_live = engine_backend_stop;
	e->sessions = session_mgr_new(&backend);
	if (!e->sessions) {
		me_set_err(err, errsz, "session manager allocation failed");
		gst_runner_free(e->runner);
		e->runner = NULL;
		return -1;
	}
	return 0;
}

void engine_deinit(Engine *e)
{
	if (!e)
		return;
	session_mgr_free(e->sessions);
	gst_runner_free(e->runner);
	aiq_ctrl_stop(&e->aiq);
}

void engine_set_event_sink(Engine *e, EngineEventCb cb, void *userdata)
{
	e->event_cb = cb;
	e->event_userdata = userdata;
}

int engine_start_live(Engine *e, const SessionParams *p, char *err,
                      size_t errsz)
{
	if (!e->aiq.started) {
		me_set_err(err, errsz, "aiq not started: %s",
		           e->aiq.error[0] ? e->aiq.error : "not started");
		return ME_ERR_MEDIA;
	}
	if (p->width != e->cfg.width || p->height != e->cfg.height ||
	    p->fps != e->cfg.fps) {
		me_set_err(err, errsz,
		           "requested %dx%d@%d does not match engine capture "
		           "%dx%d@%d",
		           p->width, p->height, p->fps, e->cfg.width, e->cfg.height,
		           e->cfg.fps);
		return ME_ERR_PARAM;
	}
	if (strcmp(p->codec, "h264") != 0) {
		me_set_err(err, errsz, "unsupported codec \"%s\" (V1 supports h264)",
		           p->codec);
		return ME_ERR_PARAM;
	}
	return session_mgr_start(e->sessions, p, err, errsz);
}

int engine_stop_live(Engine *e, const char *session_id, char *err,
                     size_t errsz)
{
	return session_mgr_stop(e->sessions, session_id, err, errsz);
}

int engine_snapshot(Engine *e, const char *channel_id, char *err,
                    size_t errsz)
{
	return gst_runner_snapshot(e->runner, channel_id, e->cfg.snapshot_dir,
	                           err, errsz);
}

int engine_get_status(Engine *e, EngineStatus *st)
{
	const MediaSession *s = session_mgr_active(e->sessions);

	st->running = s != NULL;
	st->fps = s ? s->params.fps : 0;
	st->bitrate = s ? s->params.bitrate : 0;
	return 0;
}
