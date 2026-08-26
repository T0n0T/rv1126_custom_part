/* LD_PRELOAD shim (test tool only): reports the object passed to
 * gst_object_unref when its refcount is already zero. */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <gst/gst.h>
#include <stdio.h>

__attribute__((constructor)) static void shim_init(void)
{
	fprintf(stderr, "SHIM loaded\n");
}

void gst_object_unref(gpointer object)
{
	static void (*real_unref)(gpointer) = NULL;

	if (!real_unref)
		real_unref = (void (*)(gpointer))dlsym(RTLD_NEXT,
		                                       "gst_object_unref");
	if (object && ((GObject *)object)->ref_count <= 1) {
		fprintf(stderr,
		        "SHIM unref addr=%p type=%s name=%s ref=%u\n",
		        object, G_OBJECT_TYPE_NAME(object),
		        GST_IS_OBJECT(object) ? GST_OBJECT_NAME(object) : "-",
		        ((GObject *)object)->ref_count);
	}
	real_unref(object);
}
