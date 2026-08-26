/* Board-side clean teardown probe (test tool only).
 * Replicates gst_runner start/stop lifecycle with refcount tracing to find
 * where the gst_object_unref critical comes from. */

#include <gst/gst.h>
#include <stdio.h>
#include <unistd.h>

static unsigned refs(gpointer obj)
{
	return obj ? ((GObject *)obj)->ref_count : 0;
}

static GstPadProbeReturn unlink_cb(GstPad *pad, GstPadProbeInfo *info,
                                   gpointer data)
{
	GstPad *sink = data;
	(void)info;
	gst_pad_unlink(pad, sink);
	printf("  [probe] unlinked\n");
	return GST_PAD_PROBE_REMOVE;
}

int main(void)
{
	GError *err = NULL;
	GstElement *pipeline;
	GstElement *tee;
	GstElement *bin;
	GstElement *q, *enc, *hp, *pay, *us;
	GstPad *tee_src, *branch_sink;
	guint probe;

	gst_init(NULL, NULL);
	pipeline = gst_parse_launch(
	    "v4l2src device=/dev/video24 io-mode=dmabuf ! "
	    "video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! "
	    "tee name=t",
	    &err);
	if (!pipeline) {
		printf("parse failed: %s\n", err ? err->message : "?");
		return 1;
	}
	tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");
	gst_element_set_state(pipeline, GST_STATE_PLAYING);

	bin = gst_bin_new("live-branch");
	q = gst_element_factory_make("queue", "branch-queue");
	enc = gst_element_factory_make("mpph264enc", "branch-enc");
	hp = gst_element_factory_make("h264parse", "branch-parse");
	pay = gst_element_factory_make("rtph264pay", "branch-pay");
	us = gst_element_factory_make("udpsink", "branch-sink");
	g_object_set(enc, "bps", 4096000u, NULL);
	g_object_set(pay, "pt", 98, "ssrc", 123456789u, "config-interval", 1,
	             NULL);
	g_object_set(us, "host", "127.0.0.1", "port", 30000, "sync", FALSE,
	             NULL);
	gst_bin_add_many(GST_BIN(bin), q, enc, hp, pay, us, NULL);
	gst_element_link_many(q, enc, hp, pay, us, NULL);
	gst_bin_add(GST_BIN(pipeline), bin);

	tee_src = gst_element_request_pad_simple(tee, "src_%u");
	branch_sink = gst_element_get_static_pad(q, "sink");
	printf("link ret=%d\n",
	       gst_pad_link_full(tee_src, branch_sink,
	                         GST_PAD_LINK_CHECK_NOTHING));
	gst_element_set_state(bin, GST_STATE_PLAYING);
	sleep(2);

	probe = gst_pad_add_probe(tee_src,
	                          GST_PAD_PROBE_TYPE_BLOCK |
	                              GST_PAD_PROBE_TYPE_IDLE,
	                          unlink_cb, branch_sink, NULL);
	g_usleep(300 * 1000);
	gst_pad_remove_probe(tee_src, probe);
	printf("refs after unlink: bin=%u tee_src=%u branch_sink=%u\n",
	       refs(bin), refs(tee_src), refs(branch_sink));

	printf("  [1] set_state NULL\n");
	gst_element_set_state(bin, GST_STATE_NULL);
	g_usleep(100 * 1000);
	printf("  [2] bin_remove\n");
	gst_bin_remove(GST_BIN(pipeline), bin);
	g_usleep(100 * 1000);
	printf("  [3] release tee pad\n");
	gst_element_release_request_pad(tee, tee_src);
	printf("  [4] unref tee pad (refs=%u)\n", refs(tee_src));
	gst_object_unref(tee_src);
	g_usleep(100 * 1000);
	printf("  [5] unref branch sink pad (refs=%u)\n", refs(branch_sink));
	gst_object_unref(branch_sink);
	g_usleep(100 * 1000);
	printf("  [6] unref branch bin (refs=%u)\n", refs(bin));
	gst_object_unref(bin);
	g_usleep(300 * 1000);
	printf("teardown done\n");
	return 0;
}
