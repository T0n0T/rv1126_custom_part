#include "gst/me_rga_rotate.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/base/gstbasetransform.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>

#include <rga/RgaApi.h>
#include <rga/drmrga.h>

#include <string.h>

/* Hardware NV12 rotation via librga (RGA2 on RV1126B, /dev/rga). The old C
 * API (rga_info_t + c_RkRgaBlit) is the same one used by the SDK's
 * gstreamer-rockchip mpp plugin, so it is known to work with this librga.
 * The element is only meaningful for dmabuf pipelines: v4l2src io-mode=dmabuf
 * upstream and kmssink downstream both hand us dmabuf fds, so RGA never
 * touches CPU memory. System memory is still supported as a fallback. */

#define ME_RGA_DEFAULT_ROTATION 0

enum {
	PROP_ROTATION = 1,
	PROP_OUT_WIDTH,
	PROP_OUT_HEIGHT,
};

struct _MeRgaRotate {
	GstBaseTransform parent;

	gint rotation; /* 0/90/180/270 clockwise */
	gint out_width;  /* 0 = keep rotated input size */
	gint out_height; /* 0 = keep rotated input size */
	GstVideoInfo in_info;
	GstVideoInfo out_info;
	gboolean rga_ready;
};

struct _MeRgaRotateClass {
	GstBaseTransformClass parent_class;
};

G_DEFINE_TYPE(MeRgaRotate, me_rga_rotate, GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate me_rga_rotate_sink_template =
    GST_STATIC_PAD_TEMPLATE(
        "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw,format=NV12,width=[1,MAX],"
                        "height=[1,MAX]"));

static GstStaticPadTemplate me_rga_rotate_src_template =
    GST_STATIC_PAD_TEMPLATE(
        "src", GST_PAD_SRC, GST_PAD_ALWAYS,
        GST_STATIC_CAPS("video/x-raw,format=NV12,width=[1,MAX],"
                        "height=[1,MAX]"));

static void me_rga_rotate_set_property(GObject *object, guint prop_id,
                                       const GValue *value,
                                       GParamSpec *pspec)
{
	MeRgaRotate *self = ME_RGA_ROTATE(object);

	switch (prop_id) {
	case PROP_ROTATION: {
		gint v = g_value_get_int(value);

		if (v != 0 && v != 90 && v != 180 && v != 270) {
			GST_WARNING_OBJECT(self,
			                   "invalid rotation %d, keeping %d",
			                   v, self->rotation);
			return;
		}
		self->rotation = v;
		break;
	}
	case PROP_OUT_WIDTH: {
		gint v = g_value_get_int(value);

		if (v != 0 && (v < 2 || (v & 1))) {
			GST_WARNING_OBJECT(self,
			                   "invalid out-width %d, keeping %d",
			                   v, self->out_width);
			return;
		}
		self->out_width = v;
		break;
	}
	case PROP_OUT_HEIGHT: {
		gint v = g_value_get_int(value);

		if (v != 0 && (v < 2 || (v & 1))) {
			GST_WARNING_OBJECT(self,
			                   "invalid out-height %d, keeping %d",
			                   v, self->out_height);
			return;
		}
		self->out_height = v;
		break;
	}
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void me_rga_rotate_get_property(GObject *object, guint prop_id,
                                       GValue *value, GParamSpec *pspec)
{
	MeRgaRotate *self = ME_RGA_ROTATE(object);

	switch (prop_id) {
	case PROP_ROTATION:
		g_value_set_int(value, self->rotation);
		break;
	case PROP_OUT_WIDTH:
		g_value_set_int(value, self->out_width);
		break;
	case PROP_OUT_HEIGHT:
		g_value_set_int(value, self->out_height);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static GstCaps *me_rga_rotate_transform_caps(GstBaseTransform *trans,
                                             GstPadDirection direction,
                                             GstCaps *caps, GstCaps *filter)
{
	MeRgaRotate *self = ME_RGA_ROTATE(trans);
	GstCaps *tmp = gst_caps_copy(caps);
	guint i;

	/* The sink->src query describes the configured output dimensions. In the
	 * reverse direction a configured scaler accepts input dimensions that are
	 * independent of those output dimensions; retaining the fixed output size
	 * here makes a fixed upstream caps filter intersect to EMPTY. */
	if (direction == GST_PAD_SINK && self->out_width && self->out_height) {
		for (i = 0; i < gst_caps_get_size(tmp); i++) {
			GstStructure *s = gst_caps_get_structure(tmp, i);

			gst_structure_set(s, "width", G_TYPE_INT, self->out_width,
			                  "height", G_TYPE_INT, self->out_height,
			                  NULL);
		}
	} else if (direction == GST_PAD_SINK &&
	           self->rotation % 180 != 0) {
		for (i = 0; i < gst_caps_get_size(tmp); i++) {
			GstStructure *s = gst_caps_get_structure(tmp, i);
			gint w = 0;
			gint h = 0;

			if (gst_structure_get_int(s, "width", &w) &&
			    gst_structure_get_int(s, "height", &h))
				gst_structure_set(s, "width", G_TYPE_INT, h,
				                  "height", G_TYPE_INT, w, NULL);
		}
	} else if (direction == GST_PAD_SRC && self->out_width &&
	           self->out_height) {
		for (i = 0; i < gst_caps_get_size(tmp); i++) {
			GstStructure *s = gst_caps_get_structure(tmp, i);

			gst_structure_set(s, "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
			                  "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
			                  NULL);
		}
	}

	if (filter) {
		GstCaps *intersect = gst_caps_intersect(tmp, filter);

		gst_caps_unref(tmp);
		tmp = intersect;
	}
	return tmp;
}

static gboolean me_rga_rotate_set_caps(GstBaseTransform *trans,
                                       GstCaps *incaps, GstCaps *outcaps)
{
	MeRgaRotate *self = ME_RGA_ROTATE(trans);

	if (!gst_video_info_from_caps(&self->in_info, incaps) ||
	    !gst_video_info_from_caps(&self->out_info, outcaps)) {
		GST_ERROR_OBJECT(self, "failed to parse video caps");
		return FALSE;
	}
	if ((self->out_width && !self->out_height) ||
	    (!self->out_width && self->out_height)) {
		GST_ERROR_OBJECT(self,
		                 "out-width and out-height must be set together");
		return FALSE;
	}
	if (self->out_width && self->out_height) {
		if (GST_VIDEO_INFO_WIDTH(&self->out_info) != self->out_width ||
		    GST_VIDEO_INFO_HEIGHT(&self->out_info) != self->out_height) {
			GST_ERROR_OBJECT(self,
			                 "negotiated output %dx%d != requested %dx%d",
			                 GST_VIDEO_INFO_WIDTH(&self->out_info),
			                 GST_VIDEO_INFO_HEIGHT(&self->out_info),
			                 self->out_width, self->out_height);
			return FALSE;
		}
	}
	if (GST_VIDEO_INFO_FORMAT(&self->in_info) != GST_VIDEO_FORMAT_NV12 ||
	    GST_VIDEO_INFO_FORMAT(&self->out_info) != GST_VIDEO_FORMAT_NV12) {
		GST_ERROR_OBJECT(self, "only NV12 is supported");
		return FALSE;
	}
	/* Never passthrough: even with rotation=0 the output must be a freshly
	 * allocated (aligned) buffer that RGA can write into. */
	gst_base_transform_set_passthrough(trans, FALSE);
	GST_INFO_OBJECT(self,
	                "negotiated %dx%d -> %dx%d rotation=%d",
	                GST_VIDEO_INFO_WIDTH(&self->in_info),
	                GST_VIDEO_INFO_HEIGHT(&self->in_info),
	                GST_VIDEO_INFO_WIDTH(&self->out_info),
	                GST_VIDEO_INFO_HEIGHT(&self->out_info),
	                self->rotation);
	return TRUE;
}

static gboolean me_rga_rotate_get_unit_size(GstBaseTransform *trans,
                                            GstCaps *caps, gsize *size)
{
	GstVideoInfo info;

	(void)trans;
	if (!gst_video_info_from_caps(&info, caps))
		return FALSE;
	*size = GST_VIDEO_INFO_SIZE(&info);
	return TRUE;
}

static gboolean me_rga_rotate_propose_allocation(GstBaseTransform *trans,
                                                 GstQuery *decide_query,
                                                 GstQuery *query)
{
	GstCaps *caps = NULL;
	GstVideoInfo info;
	GstBufferPool *pool;
	GstStructure *config;
	GstAllocator *allocator = NULL;
	GstAllocationParams params = { 0 };
	GstVideoAlignment align;
	gsize size;

	params.align = 15; /* 16-byte buffer/stride alignment for RGA + DRM */

	if (!GST_BASE_TRANSFORM_CLASS(me_rga_rotate_parent_class)
	         ->propose_allocation(trans, decide_query, query))
		return FALSE;
	/* Passthrough: downstream is responsible for allocation. */
	if (decide_query == NULL)
		return TRUE;

	gst_query_parse_allocation(query, &caps, NULL);
	if (caps == NULL)
		return FALSE;
	if (!gst_video_info_from_caps(&info, caps))
		return FALSE;
	size = GST_VIDEO_INFO_SIZE(&info);

	if (gst_query_get_n_allocation_pools(query) == 0) {
		/* Prefer dmabuf so kmssink gets a zero-copy framebuffer; fall back
		 * to whatever the downstream/upstream chain suggested. */
		if (gst_query_get_n_allocation_params(query) > 0) {
			GstAllocator *suggested = NULL;

			gst_query_parse_nth_allocation_param(query, 0, &suggested,
			                                     &params);
			if (suggested)
				allocator = GST_ALLOCATOR(gst_object_ref(suggested));
		}
		if (allocator == NULL)
			allocator = gst_allocator_find("dmabuf");
		if (allocator == NULL)
			allocator = gst_dmabuf_allocator_new();
		if (allocator == NULL) {
			GST_WARNING_OBJECT(trans, "no dmabuf allocator available");
			gst_query_add_allocation_param(query, NULL, &params);
		} else {
			gst_query_add_allocation_param(query, allocator, &params);
		}

		pool = gst_video_buffer_pool_new();
		config = gst_buffer_pool_get_config(pool);
		gst_buffer_pool_config_set_params(config, caps, size, 0, 0);
		gst_buffer_pool_config_set_allocator(config, allocator, &params);
		gst_buffer_pool_config_add_option(
		    config, GST_BUFFER_POOL_OPTION_VIDEO_META);
		gst_buffer_pool_config_add_option(
		    config, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
		gst_video_alignment_reset(&align);
		align.stride_align[0] = 15;
		align.stride_align[1] = 15;
		gst_buffer_pool_config_set_video_alignment(config, &align);
		if (!gst_buffer_pool_set_config(pool, config)) {
			GST_ERROR_OBJECT(trans, "failed to configure output pool");
			if (allocator)
				gst_object_unref(allocator);
			gst_object_unref(pool);
			return FALSE;
		}
		gst_query_add_allocation_pool(query, pool, size, 0, 0);
		gst_query_add_allocation_meta(query, GST_VIDEO_META_API_TYPE, NULL);
		if (allocator)
			gst_object_unref(allocator);
		gst_object_unref(pool);
	}
	return TRUE;
}

static gboolean me_rga_rotate_decide_allocation(GstBaseTransform *trans,
                                                GstQuery *query)
{
	GstBufferPool *pool = NULL;
	GstStructure *config;
	GstCaps *outcaps = NULL;
	guint size = 0, min = 0, max = 0;
	GstVideoAlignment align;

	if (gst_query_get_n_allocation_pools(query) > 0)
		gst_query_parse_nth_allocation_pool(query, 0, &pool, &size, &min,
		                                    &max);
	if (pool == NULL)
		pool = gst_video_buffer_pool_new();
	else
		gst_object_ref(pool);

	config = gst_buffer_pool_get_config(pool);
	gst_query_parse_allocation(query, &outcaps, NULL);
	if (outcaps)
		gst_buffer_pool_config_set_params(config, outcaps, size, min, max);
	gst_buffer_pool_config_add_option(config,
	                                  GST_BUFFER_POOL_OPTION_VIDEO_META);
	gst_buffer_pool_config_add_option(
	    config, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
	gst_video_alignment_reset(&align);
	align.stride_align[0] = 15;
	align.stride_align[1] = 15;
	gst_buffer_pool_config_set_video_alignment(config, &align);
	if (!gst_buffer_pool_set_config(pool, config))
		GST_WARNING_OBJECT(trans, "could not update downstream pool config");
	gst_query_set_nth_allocation_pool(query, 0, pool, size, min, max);
	gst_object_unref(pool);

	return GST_BASE_TRANSFORM_CLASS(me_rga_rotate_parent_class)
	    ->decide_allocation(trans, query);
}

static int me_rga_rotation_to_rga(gint rotation)
{
	switch (rotation) {
	case 0:
		return 0;
	case 90:
		return HAL_TRANSFORM_ROT_90;
	case 180:
		return HAL_TRANSFORM_ROT_180;
	case 270:
		return HAL_TRANSFORM_ROT_270;
	default:
		return -1;
	}
}

static GstFlowReturn me_rga_rotate_transform(GstBaseTransform *trans,
                                             GstBuffer *inbuf,
                                             GstBuffer *outbuf)
{
	MeRgaRotate *self = ME_RGA_ROTATE(trans);
	rga_info_t src = { 0 };
	rga_info_t dst = { 0 };
	GstMemory *in_mem = NULL;
	GstMemory *out_mem = NULL;
	GstVideoMeta *in_meta = NULL;
	GstVideoMeta *out_meta = NULL;
	GstMapInfo in_map = { 0 };
	GstMapInfo out_map = { 0 };
	gint in_wstride, in_hstride;
	gint out_wstride, out_hstride;
	gint in_w, in_h, out_w, out_h;
	gint rga_rotation;
	int ret;

	if (!self->rga_ready) {
		if (c_RkRgaInit() < 0) {
			g_printerr("rgarotate: RGA init failed\n");
			GST_ERROR_OBJECT(self, "RGA init failed");
			return GST_FLOW_ERROR;
		}
		self->rga_ready = TRUE;
	}

	in_w = GST_VIDEO_INFO_WIDTH(&self->in_info);
	in_h = GST_VIDEO_INFO_HEIGHT(&self->in_info);
	out_w = GST_VIDEO_INFO_WIDTH(&self->out_info);
	out_h = GST_VIDEO_INFO_HEIGHT(&self->out_info);
	if (in_w <= 0 || in_h <= 0 || out_w <= 0 || out_h <= 0 ||
	    (in_w & 1) || (in_h & 1) || (out_w & 1) || (out_h & 1)) {
		g_printerr("rgarotate: invalid NV12 dimensions %dx%d -> %dx%d\n",
		           in_w, in_h, out_w, out_h);
		GST_ERROR_OBJECT(self, "RGA NV12 requires even dimensions "
		                       "(%dx%d -> %dx%d)",
		                 in_w, in_h, out_w, out_h);
		return GST_FLOW_ERROR;
	}

	in_meta = gst_buffer_get_video_meta(inbuf);
	out_meta = gst_buffer_get_video_meta(outbuf);
	in_wstride = in_meta ? (gint)in_meta->stride[0]
	                     : GST_VIDEO_INFO_PLANE_STRIDE(&self->in_info, 0);
	out_wstride = out_meta ? (gint)out_meta->stride[0]
	                       : GST_VIDEO_INFO_PLANE_STRIDE(&self->out_info, 0);
	in_hstride = in_meta && in_wstride > 0
	                 ? (gint)(in_meta->offset[1] / (guint)in_wstride)
	                 : in_h;
	out_hstride = out_meta && out_wstride > 0
	                  ? (gint)(out_meta->offset[1] / (guint)out_wstride)
	                  : out_h;
	if (in_hstride <= 0 || out_hstride <= 0) {
		g_printerr("rgarotate: invalid stride in=%dx%d out=%dx%d\n",
		           in_wstride, in_hstride, out_wstride, out_hstride);
		GST_ERROR_OBJECT(self, "invalid stride (in %dx%d, out %dx%d)",
		                 in_wstride, in_hstride, out_wstride, out_hstride);
		return GST_FLOW_ERROR;
	}

	rga_rotation = me_rga_rotation_to_rga(self->rotation);
	if (rga_rotation < 0) {
		g_printerr("rgarotate: unsupported rotation %d\n", self->rotation);
		GST_ERROR_OBJECT(self, "unsupported rotation %d", self->rotation);
		return GST_FLOW_ERROR;
	}

	if (gst_buffer_n_memory(inbuf) == 1) {
		in_mem = gst_buffer_peek_memory(inbuf, 0);
		if (gst_is_dmabuf_memory(in_mem))
			src.fd = gst_dmabuf_memory_get_fd(in_mem);
	}
	if (src.fd <= 0) {
		if (!gst_buffer_map(inbuf, &in_map, GST_MAP_READ)) {
			g_printerr("rgarotate: cannot map input buffer\n");
			GST_ERROR_OBJECT(self, "cannot map input buffer");
			return GST_FLOW_ERROR;
		}
		src.virAddr = in_map.data;
	}

	if (gst_buffer_n_memory(outbuf) == 1) {
		out_mem = gst_buffer_peek_memory(outbuf, 0);
		if (gst_is_dmabuf_memory(out_mem))
			dst.fd = gst_dmabuf_memory_get_fd(out_mem);
	}
	if (dst.fd <= 0) {
		if (!gst_buffer_map(outbuf, &out_map, GST_MAP_WRITE)) {
			if (in_map.data)
				gst_buffer_unmap(inbuf, &in_map);
			g_printerr("rgarotate: cannot map output buffer\n");
			GST_ERROR_OBJECT(self, "cannot map output buffer");
			return GST_FLOW_ERROR;
		}
		dst.virAddr = out_map.data;
	}

	src.mmuFlag = 1;
	dst.mmuFlag = 1;
	src.rotation = rga_rotation;
	rga_set_rect(&src.rect, 0, 0, in_w, in_h, in_wstride, in_hstride,
	             RK_FORMAT_YCbCr_420_SP);
	rga_set_rect(&dst.rect, 0, 0, out_w, out_h, out_wstride, out_hstride,
	             RK_FORMAT_YCbCr_420_SP);

	ret = c_RkRgaBlit(&src, &dst, NULL);
	if (in_map.data)
		gst_buffer_unmap(inbuf, &in_map);
	if (out_map.data)
		gst_buffer_unmap(outbuf, &out_map);
	if (ret < 0) {
		g_printerr("rgarotate: RGA blit failed ret=%d in=%dx%d stride=%dx%d "
		           "out=%dx%d stride=%dx%d rotation=%d src_fd=%d dst_fd=%d\n",
		           ret, in_w, in_h, in_wstride, in_hstride, out_w, out_h,
		           out_wstride, out_hstride, self->rotation, src.fd, dst.fd);
		GST_ERROR_OBJECT(self, "RGA blit failed: %d", ret);
		return GST_FLOW_ERROR;
	}
	return GST_FLOW_OK;
}

static gboolean me_rga_rotate_start(GstBaseTransform *trans)
{
	MeRgaRotate *self = ME_RGA_ROTATE(trans);

	self->rga_ready = FALSE;
	GST_INFO_OBJECT(self, "rgarotate started (rotation=%d)", self->rotation);
	return TRUE;
}

static void me_rga_rotate_class_init(MeRgaRotateClass *klass)
{
	GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
	GstBaseTransformClass *btrans_class = GST_BASE_TRANSFORM_CLASS(klass);
	GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

	gobject_class->set_property = me_rga_rotate_set_property;
	gobject_class->get_property = me_rga_rotate_get_property;

	g_object_class_install_property(
	    gobject_class, PROP_ROTATION,
	    g_param_spec_int("rotation", "Rotation",
	                     "Clockwise rotation in degrees (0/90/180/270)",
	                     0, 270, ME_RGA_DEFAULT_ROTATION,
	                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
	    gobject_class, PROP_OUT_WIDTH,
	    g_param_spec_int("out-width", "Output width",
	                     "Scaled output width (0 = keep rotated size, even)",
	                     0, 8192, 0,
	                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
	g_object_class_install_property(
	    gobject_class, PROP_OUT_HEIGHT,
	    g_param_spec_int("out-height", "Output height",
	                     "Scaled output height (0 = keep rotated size, even)",
	                     0, 8192, 0,
	                     G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

	gst_element_class_set_static_metadata(
	    element_class, "RGA rotate", "Filter/Converter/Video",
	    "NV12 hardware rotation via librga",
	    "media_engine");
	gst_element_class_add_static_pad_template(
	    element_class, &me_rga_rotate_sink_template);
	gst_element_class_add_static_pad_template(
	    element_class, &me_rga_rotate_src_template);

	btrans_class->transform_caps = me_rga_rotate_transform_caps;
	btrans_class->set_caps = me_rga_rotate_set_caps;
	btrans_class->get_unit_size = me_rga_rotate_get_unit_size;
	btrans_class->propose_allocation = me_rga_rotate_propose_allocation;
	btrans_class->decide_allocation = me_rga_rotate_decide_allocation;
	btrans_class->transform = me_rga_rotate_transform;
	btrans_class->start = me_rga_rotate_start;
}

static void me_rga_rotate_init(MeRgaRotate *self)
{
	self->rotation = ME_RGA_DEFAULT_ROTATION;
	self->out_width = 0;
	self->out_height = 0;
	gst_video_info_init(&self->in_info);
	gst_video_info_init(&self->out_info);
	self->rga_ready = FALSE;
}

void me_rga_rotate_register(void)
{
	if (gst_element_register(NULL, "rgarotate", GST_RANK_NONE,
	                         me_rga_rotate_get_type()))
		g_debug("rgarotate element registered");
	else
		g_warning("failed to register rgarotate element");
}
