/* Board-side gst_parse_launch probe (test tool only).
 * Prints whether each argv pipeline parses, and for v4l2src what device
 * property value the parser produced. */

#include <gst/gst.h>
#include <stdio.h>

static void pad_cb(GstElement *element, GstPad *pad, gpointer data)
{
	(void)element;
	(void)data;
	printf("    pad %s (%s)\n", GST_PAD_NAME(pad),
	       GST_PAD_IS_SRC(pad) ? "src" : "sink");
}

static void link_cb(GstElement *element, GstPad *pad, gpointer data)
{
	GstPad *peer;
	(void)data;

	if (!GST_PAD_IS_SRC(pad))
		return;
	peer = gst_pad_get_peer(pad);
	if (peer) {
		GstElement *parent = gst_pad_get_parent_element(peer);
		printf("    %s.%s -> %s.%s\n", GST_OBJECT_NAME(element),
		       GST_PAD_NAME(pad),
		       parent ? GST_OBJECT_NAME(parent) : "(none)",
		       GST_PAD_NAME(peer));
		if (parent)
			gst_object_unref(parent);
		gst_object_unref(peer);
	}
}

int main(int argc, char **argv)
{
	int i;

	gst_init(NULL, NULL);
	for (i = 1; i < argc; i++) {
		GError *err = NULL;
		GstElement *bin = gst_parse_launch(argv[i], &err);
		printf("variant %d: %s\n", i,
		       bin ? "PARSE_OK" : (err ? err->message : "NULL"));
		if (bin) {
			printf("  bin type=%s name=%s is_bin=%d\n",
			       G_OBJECT_TYPE_NAME(bin), GST_OBJECT_NAME(bin),
			       GST_IS_BIN(bin));
			printf("  ghost sink=%p src=%p\n",
			       (void *)gst_element_get_static_pad(bin, "sink"),
			       (void *)gst_element_get_static_pad(bin, "src"));
			gst_element_foreach_pad(bin, pad_cb, NULL);
			gst_element_foreach_pad(bin, link_cb, NULL);
			GstIterator *it = gst_bin_iterate_elements(GST_BIN(bin));
			GValue item = G_VALUE_INIT;
			while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
				GstElement *el = (GstElement *)g_value_get_object(&item);
				if (el) {
					printf("  element %s (%s)\n", GST_OBJECT_NAME(el),
					       G_OBJECT_TYPE_NAME(el));
					if (g_object_class_find_property(
					        G_OBJECT_GET_CLASS(el), "device")) {
						gchar *dev = NULL;
						g_object_get(el, "device", &dev, NULL);
						printf("  device=[%s]\n",
						       dev ? dev : "(null)");
						g_free(dev);
					}
				}
				g_value_reset(&item);
			}
			gst_iterator_free(it);
			gst_object_unref(bin);
		}
		if (err)
			g_error_free(err);
	}
	return 0;
}
