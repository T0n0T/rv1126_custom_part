/* Board-side tee->queue link probe (test tool only). */

#include <gst/gst.h>
#include <stdio.h>
#include <unistd.h>

static void dump_pad(const char *label, GstPad *pad)
{
	GstCaps *caps = gst_pad_get_current_caps(pad);
	GstCaps *tpl = NULL;
	GstPadTemplate *t = gst_pad_get_pad_template(pad);
	gchar *c1, *c2;

	if (t)
		tpl = gst_pad_template_get_caps(t);
	c1 = caps ? gst_caps_to_string(caps) : g_strdup("(none)");
	c2 = tpl ? gst_caps_to_string(tpl) : g_strdup("(none)");
	printf("%s pad=%s linked=%d caps=%s template=%s\n", label,
	       GST_PAD_NAME(pad), gst_pad_is_linked(pad), c1, c2);
	g_free(c1);
	g_free(c2);
	if (caps)
		gst_caps_unref(caps);
	if (tpl)
		gst_caps_unref(tpl);
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
	GstElement *queue;
	GstPad *src;
	GstPad *sink;
	GstPadLinkReturn ret;

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
	queue = gst_element_factory_make("queue", "q");
	printf("tee=%p queue=%p\n", (void *)tee, (void *)queue);

	src = gst_element_request_pad_simple(tee, "src_%u");
	sink = gst_element_get_static_pad(queue, "sink");
	printf("src=%p sink=%p\n", (void *)src, (void *)sink);
	dump_pad("tee-src", src);
	dump_pad("q-sink", sink);

	ret = gst_pad_link(src, sink);
	printf("gst_pad_link ret=%d (%s)\n", ret,
	       ret == GST_PAD_LINK_OK ? "OK" : "REFUSED/NOT_LINKED");
	ret = gst_element_link(tee, queue);
	printf("gst_element_link ret=%d\n", ret);

	/* Control: programmatic tee + queue inside a shared bin. */
	{
		GstElement *bin = gst_bin_new("probe-bin");
		GstElement *tee2 = gst_element_factory_make("tee", "t2");
		GstElement *q2 = gst_element_factory_make("queue", "q2");
		GstPad *s2, *k2;

		gst_bin_add_many(GST_BIN(bin), tee2, q2, NULL);
		s2 = gst_element_request_pad_simple(tee2, "src_%u");
		k2 = gst_element_get_static_pad(q2, "sink");
		printf("control pads s2=%p k2=%p\n", (void *)s2, (void *)k2);
		printf("control gst_pad_link ret=%d\n", gst_pad_link(s2, k2));
		printf("control gst_element_link ret=%d\n",
		       gst_element_link(tee2, q2));
		gst_object_unref(s2);
		gst_object_unref(k2);
		gst_object_unref(bin);
	}
	/* Cross-bin control: tee in binA, queue in binB. */
	{
		GstElement *binA = gst_bin_new("probe-binA");
		GstElement *binB = gst_bin_new("probe-binB");
		GstElement *tee3 = gst_element_factory_make("tee", "t3");
		GstElement *q3 = gst_element_factory_make("queue", "q3");
		GstPad *s3, *k3;

		gst_bin_add(GST_BIN(binA), tee3);
		gst_bin_add(GST_BIN(binB), q3);
		s3 = gst_element_request_pad_simple(tee3, "src_%u");
		k3 = gst_element_get_static_pad(q3, "sink");
		printf("cross-bin gst_pad_link ret=%d\n", gst_pad_link(s3, k3));
		gst_object_unref(s3);
		gst_object_unref(k3);
		gst_object_unref(binA);
		gst_object_unref(binB);
	}
	/* Full replication: parse pipeline PLAYING, branch bin added to it. */
	{
		GstElement *bin = gst_bin_new("live-branch");
		GstElement *q = gst_element_factory_make("queue", "branch-queue");
		GstPad *s4, *k4;

		gst_element_set_state(pipeline, GST_STATE_PLAYING);
		gst_bin_add_many(GST_BIN(bin), q, NULL);
		gst_bin_add(GST_BIN(pipeline), bin);
		s4 = gst_element_request_pad_simple(tee, "src_%u");
		k4 = gst_element_get_static_pad(q, "sink");
		printf("replica gst_pad_link ret=%d\n", gst_pad_link(s4, k4));
		printf("replica link_full NOTHING ret=%d\n",
		       gst_pad_link_full(s4, k4, GST_PAD_LINK_CHECK_NOTHING));
		printf("replica link_full DEFAULT ret=%d\n",
		       gst_pad_link_full(s4, k4, GST_PAD_LINK_CHECK_DEFAULT));
		gst_object_unref(s4);
		gst_object_unref(k4);
	}
	/* Full live-branch replica with teardown markers. */
	{
		GstElement *bin = gst_bin_new("live-branch");
		GstElement *q = gst_element_factory_make("queue", "branch-queue");
		GstElement *enc = gst_element_factory_make("mpph264enc",
		                                          "branch-enc");
		GstElement *hp = gst_element_factory_make("h264parse",
		                                         "branch-parse");
		GstElement *pay = gst_element_factory_make("rtph264pay",
		                                          "branch-pay");
		GstElement *us = gst_element_factory_make("udpsink", "branch-sink");
		GstPad *s5, *k5;
		guint probe;

		g_object_set(enc, "bps", 4096000u, NULL);
		g_object_set(pay, "pt", 98, "ssrc", 123456789u, "config-interval",
		             1, NULL);
		g_object_set(us, "host", "127.0.0.1", "port", 30000, "sync", FALSE,
		             NULL);
		gst_bin_add_many(GST_BIN(bin), q, enc, hp, pay, us, NULL);
		gst_element_link_many(q, enc, hp, pay, us, NULL);
		gst_bin_add(GST_BIN(pipeline), bin);
		s5 = gst_element_request_pad_simple(tee, "src_%u");
		k5 = gst_element_get_static_pad(q, "sink");
		printf("replica2 link ret=%d\n",
		       gst_pad_link_full(s5, k5, GST_PAD_LINK_CHECK_NOTHING));
		gst_element_set_state(bin, GST_STATE_PLAYING);
		sleep(2);
		printf("  [teardown] add probe\n");
		probe = gst_pad_add_probe(s5, GST_PAD_PROBE_TYPE_BLOCK |
		                                  GST_PAD_PROBE_TYPE_IDLE,
		                          unlink_cb, k5, NULL);
		g_usleep(500 * 1000);
		gst_pad_remove_probe(s5, probe);
		printf("  [teardown] set_state NULL\n");
		gst_element_set_state(bin, GST_STATE_NULL);
		printf("  [teardown] bin_remove\n");
		gst_bin_remove(GST_BIN(pipeline), bin);
		printf("  [teardown] release tee pad\n");
		gst_element_release_request_pad(tee, s5);
		printf("  [teardown] unref tee pad\n");
		gst_object_unref(s5);
		printf("  [teardown] unref branch sink pad\n");
		gst_object_unref(k5);
		printf("  [teardown] unref branch bin\n");
		gst_object_unref(bin);
		printf("  [teardown] done\n");
		g_usleep(500 * 1000);
	}
	return 0;
}
