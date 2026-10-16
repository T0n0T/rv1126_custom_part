#include <gst/gst.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static GstElement *async_branch;
static GstStateChangeReturn test_gst_element_set_state(GstElement *element,
													   GstState state);

#define gst_element_set_state test_gst_element_set_state
#include "../src/gst/gst_runner.c"
#undef gst_element_set_state

static GstStateChangeReturn test_gst_element_set_state(GstElement *element,
													   GstState state)
{
	if (element == async_branch && state == GST_STATE_NULL)
		return GST_STATE_CHANGE_ASYNC;
	return gst_element_set_state(element, state);
}

void me_log(int level, const char *fmt, ...)
{
	(void)level;
	(void)fmt;
}

void me_set_err(char *buf, size_t cap, const char *fmt, ...)
{
	va_list ap;

	if (!buf || cap == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(buf, cap, fmt, ap);
	va_end(ap);
}

void me_rockiva_runner_set_stream(MeRockivaRunner *runner,
								  const char *channel_id,
								  uint64_t stream_epoch)
{
	(void)runner;
	(void)channel_id;
	(void)stream_epoch;
}

static int fail(const char *message)
{
	fprintf(stderr, "gst_runner_lifecycle_test: %s\n", message);
	return 1;
}

static bool wait_for_branch_destroy(GstRunner *runner, gint64 timeout_us)
{
	gint64 deadline = g_get_monotonic_time() + timeout_us;

	while (runner->live.branch && g_get_monotonic_time() < deadline) {
		while (g_main_context_iteration(NULL, FALSE))
			;
		g_usleep(1000);
	}
	return runner->live.branch == NULL;
}

int main(void)
{
	GstRunner runner = {0};
	GstElement *source;
	GstElement *queue;
	GstElement *sink;
	GstBus *bus;
	GstState state;
	GstStateChangeReturn state_ret;
	char err[128] = {0};

	gst_init(NULL, NULL);
	runner.pipeline = gst_pipeline_new("lifecycle-test");
	source = gst_element_factory_make("videotestsrc", "source");
	runner.tee = gst_element_factory_make("tee", "t");
	runner.live.branch = gst_bin_new("live-branch");
	queue = gst_element_factory_make("queue", "branch-queue");
	sink = gst_element_factory_make("fakesink", "branch-sink");
	if (!runner.pipeline || !source || !runner.tee || !runner.live.branch ||
	    !queue || !sink)
		return fail("required host GStreamer elements are unavailable");

	g_object_set(source, "is-live", TRUE, NULL);
	g_object_set(sink, "sync", FALSE, NULL);
	g_object_set(runner.tee, "allow-not-linked", TRUE, NULL);
	if (!gst_bin_add(GST_BIN(runner.pipeline), source) ||
	    !gst_bin_add(GST_BIN(runner.pipeline), runner.tee) ||
	    !gst_element_link(source, runner.tee))
		return fail("failed to create source-to-tee pipeline");
	gst_bin_add_many(GST_BIN(runner.live.branch), queue, sink, NULL);
	if (!gst_element_link(queue, sink) ||
	    !gst_bin_add(GST_BIN(runner.pipeline), runner.live.branch))
		return fail("failed to create live branch bin");
	gst_object_ref(runner.tee);
	gst_object_ref(runner.live.branch);
	runner.live.tee_src = gst_element_request_pad_simple(runner.tee, "src_%u");
	runner.live.branch_sink = gst_element_get_static_pad(queue, "sink");
	if (!runner.live.tee_src || !runner.live.branch_sink ||
	    gst_pad_link_full(runner.live.tee_src, runner.live.branch_sink,
	                      GST_PAD_LINK_CHECK_NOTHING) != GST_PAD_LINK_OK)
		return fail("failed to connect live branch to tee");
	strcpy(runner.live.session_id, "async-stop");
	async_branch = runner.live.branch;
	runner.branch_active = true;
	runner.live.phase = LIVE_PHASE_ACTIVE;
	runner.base_ready = true;

	state_ret = gst_element_set_state(runner.pipeline, GST_STATE_PLAYING);
	if (state_ret == GST_STATE_CHANGE_FAILURE ||
	    gst_element_get_state(runner.pipeline, &state, NULL, 2 * GST_SECOND) ==
	        GST_STATE_CHANGE_FAILURE)
		return fail("test pipeline failed to enter PLAYING");
	bus = gst_element_get_bus(runner.pipeline);
	runner.bus_watch = gst_bus_add_watch(bus, runner_bus_cb, &runner);
	gst_object_unref(bus);

	if (gst_runner_stop_live(&runner, "async-stop", err, sizeof(err)) !=
	    ME_ERR_OK)
		return fail("stop request failed");
	if (!runner.live.branch)
		return fail("branch was destroyed before its async NULL transition");
	if (runner.live.teardown_idle)
		return fail("destroy idle was scheduled before the NULL state message");
	if (runner.live.phase != LIVE_PHASE_WAITING_NULL)
		return fail("stop did not enter WAITING_NULL for an async transition");
	if (runner.branch_active)
		return fail("branch remained active after stop was accepted");
	if (gst_runner_stop_live(&runner, "async-stop", err, sizeof(err)) !=
	    ME_ERR_OK)
		return fail("duplicate stop was not idempotent while waiting for NULL");
	if (runner.live.teardown_idle)
		return fail("duplicate stop scheduled destruction before NULL");

	state_ret = gst_element_set_state(runner.live.branch, GST_STATE_NULL);
	if (state_ret == GST_STATE_CHANGE_FAILURE)
		return fail("deferred NULL transition failed");
	if (!wait_for_branch_destroy(&runner, 2 * G_TIME_SPAN_SECOND))
		return fail("branch was not destroyed after the NULL bus message");
	if (runner.live.phase != LIVE_PHASE_DESTROYED)
		return fail("branch teardown did not reach DESTROYED");

	if (runner.bus_watch)
		g_source_remove(runner.bus_watch);
	gst_element_set_state(runner.pipeline, GST_STATE_NULL);
	gst_object_unref(runner.tee);
	gst_object_unref(runner.pipeline);
	puts("gst_runner_lifecycle_test: PASS");
	return 0;
}
