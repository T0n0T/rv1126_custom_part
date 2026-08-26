#include "gst/gst_runner.h"
#include "gst/me_rga_rotate.h"

#include "common/me_errors.h"
#include "common/util.h"

#include <gst/gst.h>
#include <glib.h>
#include <stdio.h>
#include <string.h>

#define ME_GST_BASE_READY_TIMEOUT (5 * GST_SECOND)

typedef struct {
	char session_id[ME_SESSION_ID_MAX];
	int fps;
	int bitrate;
	GstPad *tee_src;     /* requested tee src pad (owned ref) */
	GstPad *branch_sink; /* branch bin sink pad (owned ref) */
	GstElement *branch;  /* branch bin (owned ref) */
	guint teardown_idle;
} LiveBranch;

struct GstRunner {
	EngineConfig cfg;
	GstElement *pipeline;
	GstElement *tee;
	guint bus_watch;
	bool base_ready;
	char base_error[256];
	LiveBranch live;
	bool branch_active;
	GstRunnerEventCb event_cb;
	void *event_userdata;
};

static void runner_emit_event(GstRunner *r, const char *event,
                              const char *session_id, const char *message)
{
	if (r->event_cb)
		r->event_cb(r->event_userdata, event, session_id, message);
}

/* ---------- base pipeline ---------- */

static GstElement *build_base_pipeline(const EngineConfig *cfg, char **err_text)
{
	GString *s = g_string_new(NULL);
	GError *gerr = NULL;
	GstElement *pipe;

	g_string_append_printf(s,
	    "v4l2src device=\"%s\" io-mode=dmabuf ! "
	    "video/x-raw,format=%s,width=%d,height=%d,framerate=%d/1 ! "
	    "tee name=t allow-not-linked=true",
	    cfg->device, cfg->format, cfg->width, cfg->height, cfg->fps);
	if (cfg->preview) {
		g_string_append(s, " t. ! queue");
		/* This board's VOP plane caps at 1920x1920 input, so RGA always
		 * scales the preview to the screen size (default 480x800) in the
		 * same pass as rotation; kmssink then displays 1:1. */
		g_string_append_printf(s,
		    " ! rgarotate rotation=%d out-width=%d out-height=%d",
		    cfg->preview_rotation, cfg->preview_width,
		    cfg->preview_height);
		g_string_append_printf(s,
		    " ! kmssink sync=false connector-id=%d plane-id=%d "
		    "skip-vsync=true",
		    cfg->connector_id, cfg->plane_id);
	} else {
		/* Keep at least one linked pad on the tee from startup: a bare
		 * tee that starts with zero pads can fail to deliver buffers to a
		 * branch requested later on this SDK build. fakesink just drops
		 * frames, so the camera keeps streaming with no display cost. */
		g_string_append(s, " t. ! fakesink sync=false");
	}

	pipe = gst_parse_launch(s->str, &gerr);
	if (!pipe) {
		*err_text =
		    g_strdup(gerr && gerr->message ? gerr->message : "parse error");
		if (gerr)
			g_error_free(gerr);
		g_string_free(s, TRUE);
		return NULL;
	}
	if (gerr) {
		me_log(ME_LOG_WARN, "base pipeline parse warning: %s", gerr->message);
		g_error_free(gerr);
	}
	g_string_free(s, TRUE);
	return pipe;
}

static gboolean runner_bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
	GstRunner *r = data;
	(void)bus;

	switch (GST_MESSAGE_TYPE(msg)) {
	case GST_MESSAGE_ERROR: {
		GError *gerr = NULL;
		gchar *debug = NULL;

		gst_message_parse_error(msg, &gerr, &debug);
		me_log(ME_LOG_ERROR, "gst error from %s: %s",
		       GST_OBJECT_NAME(msg->src),
		       gerr && gerr->message ? gerr->message : "unknown error");
		if (debug)
			me_log(ME_LOG_DEBUG, "gst debug: %s", debug);
		runner_emit_event(r, "error",
		                   r->branch_active ? r->live.session_id : NULL,
		                   gerr && gerr->message ? gerr->message
		                                         : "gstreamer error");
		r->base_ready = false;
		if (gerr)
			g_error_free(gerr);
		g_free(debug);
		break;
	}
	case GST_MESSAGE_EOS:
		me_log(ME_LOG_INFO, "gst EOS");
		runner_emit_event(r, "eos",
		                   r->branch_active ? r->live.session_id : NULL,
		                   "pipeline EOS");
		break;
	case GST_MESSAGE_STATE_CHANGED: {
		GstState old_state, new_state, pending;
		gst_message_parse_state_changed(msg, &old_state, &new_state,
		                                &pending);
		(void)old_state;
		(void)pending;
		if ((GstObject *)r->pipeline == GST_MESSAGE_SRC(msg) &&
		    new_state == GST_STATE_PLAYING)
			me_log(ME_LOG_INFO, "base pipeline PLAYING");
		break;
	}
	default:
		break;
	}
	return G_SOURCE_CONTINUE;
}

/* ---------- live branch ---------- */

static bool has_property(GstElement *el, const char *name)
{
	return g_object_class_find_property(G_OBJECT_GET_CLASS(el), name) != NULL;
}

/* Builds the encode branch programmatically: the gstreamer-rockchip plugin
 * exposes the bitrate as "bps" (bit/s), and gst_parse_launch silently drops
 * elements when a property name is wrong, so we construct and validate
 * explicitly. */
static GstElement *create_live_branch(const SessionParams *p, int rotation,
                                      char *err, size_t errsz)
{
	GstElement *bin = NULL;
	GstElement *queue = NULL;
	GstElement *rot = NULL;
	GstElement *enc = NULL;
	GstElement *parse = NULL;
	GstElement *pay = NULL;
	GstElement *sink = NULL;
	bool ok = false;

	bin = gst_bin_new("live-branch");
	queue = gst_element_factory_make("queue", "branch-queue");
	enc = gst_element_factory_make("mpph264enc", "branch-enc");
	parse = gst_element_factory_make("h264parse", "branch-parse");
	pay = gst_element_factory_make("rtph264pay", "branch-pay");
	sink = gst_element_factory_make("udpsink", "branch-sink");
	if (!bin || !queue || !enc || !parse || !pay || !sink) {
		me_set_err(err, errsz, "create live branch element failed");
		goto out;
	}
	if (rotation) {
		rot = gst_element_factory_make("rgarotate", "branch-rot");
		if (!rot) {
			me_set_err(err, errsz,
			           "create rgarotate element failed (rotation requested)");
			goto out;
		}
		if (!has_property(rot, "rotation")) {
			me_set_err(err, errsz, "rgarotate missing rotation property");
			goto out;
		}
		g_object_set(rot, "rotation", rotation, NULL);
	}
	if (!has_property(enc, "bps") || !has_property(pay, "pt") ||
	    !has_property(pay, "ssrc") || !has_property(pay, "config-interval") ||
	    !has_property(sink, "host") || !has_property(sink, "port") ||
	    !has_property(sink, "sync")) {
		me_set_err(err, errsz,
		           "live branch element is missing required properties "
		           "(mpph264enc bps / rtph264pay pt,ssrc,config-interval / "
		           "udpsink host,port,sync)");
		goto out;
	}
	if ((guint)p->bitrate > G_MAXUINT / 1000u) {
		me_set_err(err, errsz, "bitrate too large: %d kbps", p->bitrate);
		goto out;
	}

	/* Session bitrate is in kbps (daemon contract); the MPP property is in
	 * bits per second. */
	/* A leaky downstream queue keeps the tee from ever blocking on a slow
	 * encoder/network, so the preview branch and stop_live stay responsive. */
	g_object_set(queue, "leaky", 2 /* downstream */, "max-size-buffers", 5,
	             NULL);
	g_object_set(enc, "bps", (guint)p->bitrate * 1000u, NULL);
	g_object_set(pay, "pt", p->payload_type, "ssrc", p->ssrc,
	             "config-interval", 1, NULL);
	g_object_set(sink, "host", p->dest_ip, "port", p->dest_port, "sync",
	             FALSE, NULL);

	gst_bin_add_many(GST_BIN(bin), queue, enc, parse, pay, sink, NULL);
	if (rot)
		gst_bin_add(GST_BIN(bin), rot);
	if (!gst_element_link(queue, rot ? rot : enc)) {
		me_set_err(err, errsz, "link queue to rotator/encoder failed");
		goto out;
	}
	if (rot && !gst_element_link(rot, enc)) {
		me_set_err(err, errsz, "link rotator to encoder failed");
		goto out;
	}
	if (!gst_element_link_many(enc, parse, pay, sink, NULL)) {
		me_set_err(err, errsz, "link live branch elements failed");
		goto out;
	}
	ok = true;

out:
	if (!ok) {
		if (bin)
			gst_object_unref(bin);
		return NULL;
	}
	return bin;
}

int gst_runner_start_live(GstRunner *r, const SessionParams *p, char *err,
                          size_t errsz)
{
	GstPad *src = NULL;
	GstPad *sink = NULL;
	GstElement *queue = NULL;
	GstPadLinkReturn link_ret;
	bool added = false;

	if (!r->base_ready) {
		me_set_err(err, errsz, "base pipeline not ready: %s",
		           r->base_error[0] ? r->base_error : "starting");
		return ME_ERR_MEDIA;
	}
	if (r->branch_active || r->live.branch) {
		me_set_err(err, errsz,
		           "live branch already active for session %s",
		           r->live.session_id);
		return ME_ERR_MEDIA;
	}
	if (strcmp(p->codec, "h264") != 0) {
		me_set_err(err, errsz, "unsupported codec \"%s\" (V1 supports h264)",
		           p->codec);
		return ME_ERR_PARAM;
	}

	r->live.branch = create_live_branch(p, r->cfg.stream_rotation, err, errsz);
	if (!r->live.branch)
		return ME_ERR_MEDIA;
	/* The branch must share the pipeline ancestor before pads can be
	 * linked (this build rejects cross-bin pad links). gst_bin_new returns
	 * a floating reference: gst_bin_add sinks it into the pipeline, so we
	 * take our own reference explicitly and drop it in destroy. */
	if (!gst_bin_add(GST_BIN(r->pipeline), r->live.branch)) {
		me_set_err(err, errsz, "add live branch to pipeline failed");
		r->live.branch = NULL; /* a failed add sinks and frees the element */
		return ME_ERR_MEDIA;
	}
	gst_object_ref(r->live.branch);
	added = true;

	queue = gst_bin_get_by_name(GST_BIN(r->live.branch), "branch-queue");
	if (!queue) {
		me_set_err(err, errsz, "live branch queue not found");
		goto fail_branch;
	}
	sink = gst_element_get_static_pad(queue, "sink");
	gst_object_unref(queue);
	queue = NULL;
	if (!sink) {
		me_set_err(err, errsz, "live branch has no sink pad");
		goto fail_branch;
	}
	src = gst_element_request_pad_simple(r->tee, "src_%u");
	if (!src) {
		me_set_err(err, errsz, "tee request pad failed");
		goto fail_branch;
	}
	/* This SDK's GStreamer build includes HIERARCHY in the default link
	 * checks, which rejects the dynamic tee->branch link even when the
	 * branch shares the pipeline ancestor. We know the pads are compatible,
	 * so skip the checks. */
	link_ret = gst_pad_link_full(src, sink, GST_PAD_LINK_CHECK_NOTHING);
	if (link_ret != GST_PAD_LINK_OK) {
		me_set_err(err, errsz, "tee pad link failed: %d", link_ret);
		goto fail_branch;
	}

	r->live.tee_src = src;
	r->live.branch_sink = sink;
	r->live.fps = p->fps;
	r->live.bitrate = p->bitrate;
	snprintf(r->live.session_id, sizeof(r->live.session_id), "%s",
	         p->session_id);

	if (gst_element_set_state(r->live.branch, GST_STATE_PLAYING) ==
	    GST_STATE_CHANGE_FAILURE) {
		me_set_err(err, errsz, "live branch failed to PLAYING");
		goto fail_branch_attached;
	}

	r->branch_active = true;
	me_log(ME_LOG_INFO,
	       "live branch %s -> %s:%d codec=%s ssrc=%u pt=%d bitrate=%d "
	       "rotation=%d",
	       p->session_id, p->dest_ip, p->dest_port, p->codec, p->ssrc,
	       p->payload_type, p->bitrate, r->cfg.stream_rotation);
	return ME_ERR_OK;

fail_branch_attached:
	gst_pad_unlink(src, sink);
	gst_element_release_request_pad(r->tee, src);
	gst_object_unref(src);
	src = NULL;
	gst_object_unref(sink);
	sink = NULL;
fail_branch:
	if (src)
		gst_object_unref(src);
	if (sink)
		gst_object_unref(sink);
	if (r->live.branch) {
		gst_element_set_state(r->live.branch, GST_STATE_NULL);
		if (added)
			gst_bin_remove(GST_BIN(r->pipeline), r->live.branch);
		gst_object_unref(r->live.branch);
		r->live.branch = NULL;
	}
	return ME_ERR_MEDIA;
}

/* Destroys the branch bin. Must run on the main loop AFTER the bus has
 * dispatched all messages the branch posted while stopping: this SDK's
 * GStreamer build can post a state-changed message whose src reference is
 * not held, so destroying the bin synchronously leaves a dangling src on the
 * bus. */
static gboolean live_branch_destroy_idle(gpointer data)
{
	GstRunner *r = data;

	r->live.teardown_idle = 0;
	if (!r->live.branch)
		return G_SOURCE_REMOVE;
	gst_bin_remove(GST_BIN(r->pipeline), r->live.branch);
	gst_object_unref(r->live.branch);
	r->live.branch = NULL;
	r->live.session_id[0] = '\0';
	r->live.fps = 0;
	r->live.bitrate = 0;
	me_log(ME_LOG_DEBUG, "gst: branch bin destroyed");
	return G_SOURCE_REMOVE;
}

static void live_branch_schedule_destroy(GstRunner *r)
{
	if (r->live.teardown_idle)
		return;
	r->live.teardown_idle = g_idle_add(live_branch_destroy_idle, r);
}

/* Stops the branch: unlinks the tee pad and releases it while holding the tee
 * sink pad's stream lock. gst_pad_push on the tee sink pad holds that lock
 * for the whole multi-pad push, so once we hold it no tee src pad is being
 * used by the streaming thread. Bin destruction is deferred to an idle
 * callback so pending bus messages are drained first. */
static void live_branch_teardown(GstRunner *r)
{
	GstPad *tee_sink;

	tee_sink = gst_element_get_static_pad(r->tee, "sink");
	if (tee_sink) {
		GST_PAD_STREAM_LOCK(tee_sink);
		if (r->live.tee_src && r->live.branch_sink)
			gst_pad_unlink(r->live.tee_src, r->live.branch_sink);
		if (r->live.tee_src) {
			gst_element_release_request_pad(r->tee, r->live.tee_src);
			gst_object_unref(r->live.tee_src);
			r->live.tee_src = NULL;
		}
		if (r->live.branch_sink) {
			gst_object_unref(r->live.branch_sink);
			r->live.branch_sink = NULL;
		}
		GST_PAD_STREAM_UNLOCK(tee_sink);
		gst_object_unref(tee_sink);
	}

	gst_element_set_state(r->live.branch, GST_STATE_NULL);
	r->branch_active = false;
	live_branch_schedule_destroy(r);
}

int gst_runner_stop_live(GstRunner *r, const char *session_id, char *err,
                         size_t errsz)
{
	if (!r->branch_active || strcmp(r->live.session_id, session_id) != 0) {
		me_set_err(err, errsz, "no live branch for session %s", session_id);
		return ME_ERR_NOT_FOUND;
	}

	live_branch_teardown(r);
	me_log(ME_LOG_INFO, "live branch %s stopped, udpsink released",
	       session_id);
	return ME_ERR_OK;
}

int gst_runner_snapshot(GstRunner *r, const char *channel_id,
                        const char *out_dir, char *err, size_t errsz)
{
	(void)r;
	(void)channel_id;
	(void)out_dir;
	me_set_err(err, errsz, "snapshot not implemented yet (deferred)");
	return ME_ERR_MEDIA;
}

void gst_runner_status(GstRunner *r, bool *running, int *fps, int *bitrate)
{
	*running = r->branch_active;
	*fps = r->branch_active ? r->live.fps : 0;
	*bitrate = r->branch_active ? r->live.bitrate : 0;
}

/* ---------- lifecycle ---------- */

GstRunner *gst_runner_new(const EngineConfig *cfg, char *err, size_t errsz)
{
	GstRunner *r;
	GstBus *bus = NULL;
	GstMessage *msg;
	gint64 deadline;
	char *parse_err = NULL;
	bool playing = false;

	r = g_new0(GstRunner, 1);
	r->cfg = *cfg;

	gst_init(NULL, NULL);
	me_rga_rotate_register();

	r->pipeline = build_base_pipeline(cfg, &parse_err);
	if (!r->pipeline) {
		g_strlcpy(r->base_error, parse_err ? parse_err : "pipeline parse error",
		          sizeof(r->base_error));
		g_free(parse_err);
		me_log(ME_LOG_ERROR, "base pipeline create failed: %s",
		       r->base_error);
		me_set_err(err, errsz, "base pipeline create failed: %s",
		           r->base_error);
		return r;
	}

	r->tee = gst_bin_get_by_name(GST_BIN(r->pipeline), "t");
	if (!r->tee) {
		g_strlcpy(r->base_error, "tee element not found in base pipeline",
		          sizeof(r->base_error));
		me_log(ME_LOG_ERROR, "%s", r->base_error);
		gst_element_set_state(r->pipeline, GST_STATE_NULL);
		gst_object_unref(r->pipeline);
		r->pipeline = NULL;
		me_set_err(err, errsz, "%s", r->base_error);
		return r;
	}

	bus = gst_element_get_bus(r->pipeline);
	gst_element_set_state(r->pipeline, GST_STATE_PLAYING);

	deadline = g_get_monotonic_time() + ME_GST_BASE_READY_TIMEOUT;
	while (!playing && !r->base_error[0]) {
		msg = gst_bus_timed_pop_filtered(
		    bus, deadline, GST_MESSAGE_ERROR | GST_MESSAGE_STATE_CHANGED);
		if (!msg)
			break; /* timeout */
		if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
			GError *gerr = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(msg, &gerr, &debug);
			g_strlcpy(r->base_error,
			          gerr && gerr->message ? gerr->message : "unknown error",
			          sizeof(r->base_error));
			if (gerr)
				g_error_free(gerr);
			g_free(debug);
			gst_message_unref(msg);
			break;
		}
		if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_STATE_CHANGED) {
			GstState old_state, new_state, pending;
			gst_message_parse_state_changed(msg, &old_state, &new_state,
			                                &pending);
			(void)old_state;
			(void)pending;
			if ((GstObject *)r->pipeline == GST_MESSAGE_SRC(msg) &&
			    new_state == GST_STATE_PLAYING)
				playing = true;
		}
		gst_message_unref(msg);
	}

	if (r->base_error[0]) {
		me_log(ME_LOG_ERROR, "base pipeline failed to start: %s",
		       r->base_error);
	} else {
		if (!playing)
			me_log(ME_LOG_WARN,
			       "base pipeline did not report PLAYING within %d s, "
			       "continuing optimistically",
			       (int)(ME_GST_BASE_READY_TIMEOUT / GST_SECOND));
		r->base_ready = true;
	}

	r->bus_watch = gst_bus_add_watch(bus, runner_bus_cb, r);
	gst_object_unref(bus);
	if (r->base_ready)
		me_log(ME_LOG_INFO,
		       "gst_runner ready: %s %dx%d@%d preview=%s "
		       "preview_rotation=%d stream_rotation=%d",
		       cfg->device, cfg->width, cfg->height, cfg->fps,
		       cfg->preview ? "on" : "off", cfg->preview_rotation,
		       cfg->stream_rotation);
	else
		me_log(ME_LOG_INFO,
		       "gst_runner created, base pipeline NOT ready (IPC usable)");
	return r;
}

void gst_runner_free(GstRunner *r)
{
	if (!r)
		return;
	if (r->live.teardown_idle) {
		g_source_remove(r->live.teardown_idle);
		r->live.teardown_idle = 0;
	}
	if (r->branch_active)
		gst_runner_stop_live(r, r->live.session_id, NULL, 0);
	if (r->live.branch) {
		if (r->live.teardown_idle) {
			g_source_remove(r->live.teardown_idle);
			r->live.teardown_idle = 0;
		}
		live_branch_destroy_idle(r);
	}
	if (r->bus_watch) {
		g_source_remove(r->bus_watch);
		r->bus_watch = 0;
	}
	if (r->pipeline) {
		gst_element_set_state(r->pipeline, GST_STATE_NULL);
		gst_object_unref(r->pipeline);
	}
	g_free(r);
}

void gst_runner_set_event_cb(GstRunner *r, GstRunnerEventCb cb,
                             void *userdata)
{
	r->event_cb = cb;
	r->event_userdata = userdata;
}
