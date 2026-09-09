#include "engine/engine.h"

#include "common/util.h"

#include <glib.h>

#include <string.h>
#include <strings.h>

typedef struct {
	Engine *engine;
	MeAnalyticsEvent event;
	uint64_t cursor;
} AnalyticsEventDispatch;

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

static gboolean deliver_analytics_event(gpointer userdata)
{
	AnalyticsEventDispatch *dispatch = userdata;
	Engine *e = dispatch->engine;

	if (e->analytics_event_cb)
		e->analytics_event_cb(e->analytics_event_userdata, dispatch->cursor,
							   &dispatch->event);
	g_free(dispatch);
	return G_SOURCE_REMOVE;
}

/* RockIVA invokes the event-engine sink from its worker thread. Copy the
 * synchronous event and hand delivery to the GLib main context so IPC state
 * remains owned by the main loop. */
static void engine_analytics_event(void *userdata, const MeAnalyticsEvent *event)
{
	Engine *e = userdata;
	AnalyticsEventDispatch *dispatch;
	MeAnalyticsEvent event_copy;
	char err[256] = {0};
	uint64_t cursor = 0;
	int rc;

	if (!e || !event)
		return;
	if (!e->event_journal) {
		me_log(ME_LOG_WARN,
		       "analytics event not delivered: durable event journal unavailable");
		return;
	}
	event_copy = *event;
	if (!strcasecmp(e->cfg.analytics.evidence_mode, "exact_evidence")) {
		rc = gst_runner_capture_evidence(e->runner, &event_copy, err,
								 sizeof(err));
		if (rc < 0)
			me_log(ME_LOG_WARN, "exact evidence capture failed: %s", err);
		else if (rc > 0)
			me_log(ME_LOG_WARN, "exact evidence unavailable for event %s: %s",
			       event->event_id, err[0] ? err : "no matching JPEG");
	}
	rc = me_event_journal_append(e->event_journal, &event_copy, &cursor, err,
							 sizeof(err));
	if (rc > 0) {
		me_log(ME_LOG_WARN, "analytics UPDATE dropped by event journal pressure");
		return;
	}
	if (rc < 0) {
		me_log(ME_LOG_ERROR, "analytics event journal append failed: %s", err);
		return;
	}
	dispatch = g_new0(AnalyticsEventDispatch, 1);
	if (!dispatch) {
		me_log(ME_LOG_ERROR, "analytics event delivery allocation failed");
		return;
	}
	dispatch->engine = e;
	dispatch->event = event_copy;
	dispatch->cursor = cursor;
	g_main_context_invoke(NULL, deliver_analytics_event, dispatch);
}

static void engine_analytics_observation(
	void *userdata, const MeNormalizedObservation *observation)
{
	Engine *e = userdata;
	char err[256];

	if (!e || !e->analytics_initialized || !observation)
		return;
	gst_runner_note_analytics_frame(e->runner, observation);
	if (me_event_engine_process(&e->analytics, observation, err, sizeof(err)) <
	    0)
		me_log(ME_LOG_WARN, "analytics observation rejected: %s", err);
}

int engine_init(Engine *e, const EngineConfig *cfg, char *err, size_t errsz)
{
	LiveBackend backend;

	memset(e, 0, sizeof(*e));
	e->cfg = *cfg;
	if (me_event_engine_init(&e->analytics, &cfg->analytics, 1,
	                         engine_analytics_event, e, err, errsz) != 0)
		return -1;
	e->analytics_initialized = true;
	if (cfg->analytics.enabled) {
		e->event_journal = me_event_journal_open(
			cfg->analytics.event_log_path,
			cfg->analytics.event_log_max_records,
			cfg->analytics.event_log_max_bytes, err, errsz);
		if (!e->event_journal) {
			me_log(ME_LOG_ERROR,
			       "analytics event journal unavailable; analytics delivery disabled: %s",
			       err && err[0] ? err : "unknown error");
			if (err && errsz > 0)
				err[0] = '\0';
		}
	}

	/* AIQ first: the sensor/ISP must be up before the capture pipeline. A
	 * failure is not fatal to the control plane; start_live reports it. */
	if (aiq_ctrl_start(&e->aiq, cfg->cam_id, cfg->iq_dir, cfg->af_mode, err,
	                   errsz) != 0)
		me_log(ME_LOG_ERROR, "aiq start failed (IPC stays usable)");

	e->runner = gst_runner_new(cfg, engine_analytics_observation, e, err,
	                           errsz);
	if (!e->runner) {
		if (!err || !err[0])
			me_set_err(err, errsz, "gst_runner allocation failed");
		(void)me_event_engine_deinit(&e->analytics, NULL, 0);
		me_event_journal_close(e->event_journal);
		e->event_journal = NULL;
		e->analytics_initialized = false;
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
		(void)me_event_engine_deinit(&e->analytics, NULL, 0);
		me_event_journal_close(e->event_journal);
		e->event_journal = NULL;
		e->analytics_initialized = false;
		return -1;
	}
	return 0;
}

void engine_deinit(Engine *e)
{
	char err[128] = {0};

	if (!e)
		return;
	session_mgr_free(e->sessions);
	gst_runner_free(e->runner);
	e->runner = NULL;
	if (e->analytics_initialized) {
		/* Close the last lifecycle before clearing the engine. The main loop
		 * drains the queued delivery in main() while the IPC server is alive. */
		(void)me_event_engine_close(&e->analytics,
						   ME_EVENT_REASON_PROCESS_RESTART, err, sizeof(err));
		if (me_event_engine_deinit(&e->analytics, err, sizeof(err)) < 0)
			me_log(ME_LOG_ERROR, "analytics engine shutdown incomplete: %s",
			       err[0] ? err : "unknown error");
		e->analytics_initialized = false;
	}
	me_event_journal_close(e->event_journal);
	e->event_journal = NULL;
	aiq_ctrl_stop(&e->aiq);
}

void engine_set_event_sink(Engine *e, EngineEventCb cb, void *userdata)
{
	e->event_cb = cb;
	e->event_userdata = userdata;
}

void engine_set_analytics_event_sink(Engine *e, EngineAnalyticsEventCb cb,
								 void *userdata)
{
	e->analytics_event_cb = cb;
	e->analytics_event_userdata = userdata;
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
