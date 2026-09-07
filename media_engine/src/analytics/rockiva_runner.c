#include "analytics/rockiva_runner.h"

#include "common/util.h"

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video.h>

#include <glib.h>

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"

#define ME_ROCKIVA_MODEL_PATH "/oem/usr/lib"
#define ME_ROCKIVA_CHANNEL_ID 0U
#define ME_ROCKIVA_CORE_MASK 0x04U
#define ME_ROCKIVA_WAIT_TIMEOUT_MS 5000
#define ME_ROCKIVA_VERSION_MAX 128U

typedef struct {
	GstBuffer *buffer;
	int32_t fd;
	GstClockTime pts;
	guint width;
	guint height;
	guint hstride;
	gint stride;
	guint64 stream_epoch;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	bool detection_seen;
	bool release_seen;
} MeRockivaFrame;

typedef struct {
	guint width;
	guint height;
	guint hstride;
	gint stride;
	gint uv_stride;
	gsize offset;
	gsize uv_offset;
	int32_t fd;
} MeRockivaLayout;

struct MeRockivaRunner {
	GMutex lock;
	GCond callbacks_cond;
	GstAppSink *appsink;
	GHashTable *frames; /* frame id -> MeRockivaFrame */
	RockIvaHandle handle;
	bool handle_initialized;
	bool detect_initialized;
	bool release_callback_initialized;
	bool stopping;
	guint active_callbacks;
	guint pending_frames;
	uint32_t next_frame_id;
	uint64_t rejected_samples;
	uint64_t stream_epoch;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	char model_version[ME_ANALYTICS_MODEL_VERSION_MAX];
	char backend_version[ME_ANALYTICS_VERSION_MAX];
	MeAnalyticsConfig config;
	MeRockivaObservationCb observation_cb;
	void *observation_userdata;
};

static void rockiva_set_err(char *err, size_t errsz, const char *message)
{
	if (err && errsz > 0)
		snprintf(err, errsz, "%s", message ? message : "RockIVA error");
}

static const char *ret_name(RockIvaRetCode ret)
{
	switch (ret) {
	case ROCKIVA_RET_SUCCESS:
		return "success";
	case ROCKIVA_RET_UNSUPPORTED:
		return "unsupported";
	case ROCKIVA_RET_BUFFER_FULL:
		return "buffer-full";
	default:
		return "failure";
	}
}

static bool model_from_name(const char *name, RockIvaDetModel *model)
{
	if (!name || !model)
		return false;
	if (!g_ascii_strcasecmp(name, "pfp") ||
	    !g_ascii_strcasecmp(name, "pfp-v1") ||
	    !g_ascii_strcasecmp(name, "pfp_v1")) {
		*model = ROCKIVA_DET_MODEL_PFP;
		return true;
	}
	if (!g_ascii_strcasecmp(name, "pfp-v3") ||
	    !g_ascii_strcasecmp(name, "pfp_v3")) {
		*model = ROCKIVA_DET_MODEL_PFP_V3;
		return true;
	}
	if (!g_ascii_strcasecmp(name, "cls8") ||
	    !g_ascii_strcasecmp(name, "cls8-v1") ||
	    !g_ascii_strcasecmp(name, "cls8_v1")) {
		*model = ROCKIVA_DET_MODEL_CLS8;
		return true;
	}
	if (!g_ascii_strcasecmp(name, "person")) {
		*model = ROCKIVA_DET_MODEL_PERSON;
		return true;
	}
	return false;
}

static bool inspect_sample(GstSample *sample, const MeAnalyticsConfig *config,
					   MeRockivaLayout *layout)
{
	GstBuffer *buffer;
	GstCaps *caps;
	GstVideoInfo video_info;
	GstVideoMeta *meta;
	GstMemory *memory;
	gsize memory_offset = 0;
	gsize memory_maxsize = 0;
	uint64_t hstride;
	uint64_t uv_size;
	uint64_t required_size;
	gsize visible_size;

	if (!sample || !config || !layout)
		return false;
	buffer = gst_sample_get_buffer(sample);
	caps = gst_sample_get_caps(sample);
	if (!buffer || !caps || gst_caps_get_size(caps) != 1 ||
	    !gst_video_info_from_caps(&video_info, caps))
		return false;
	if (GST_VIDEO_INFO_FORMAT(&video_info) != GST_VIDEO_FORMAT_NV12 ||
	    (guint)GST_VIDEO_INFO_WIDTH(&video_info) != (guint)config->width ||
	    (guint)GST_VIDEO_INFO_HEIGHT(&video_info) != (guint)config->height ||
	    GST_VIDEO_INFO_INTERLACE_MODE(&video_info) !=
		GST_VIDEO_INTERLACE_MODE_PROGRESSIVE ||
	    GST_VIDEO_INFO_N_PLANES(&video_info) != 2 ||
	    gst_buffer_n_memory(buffer) != 1)
		return false;

	memory = gst_buffer_peek_memory(buffer, 0);
	if (!memory || !gst_is_dmabuf_memory(memory))
		return false;

	memset(layout, 0, sizeof(*layout));
	layout->width = GST_VIDEO_INFO_WIDTH(&video_info);
	layout->height = GST_VIDEO_INFO_HEIGHT(&video_info);
	layout->stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 0);
	layout->uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 1);
	layout->offset = GST_VIDEO_INFO_PLANE_OFFSET(&video_info, 0);
	layout->uv_offset = GST_VIDEO_INFO_PLANE_OFFSET(&video_info, 1);
	meta = gst_buffer_get_video_meta(buffer);
	if (meta) {
		if (meta->format != GST_VIDEO_FORMAT_NV12 || meta->n_planes != 2 ||
		    meta->width != layout->width || meta->height != layout->height)
			return false;
		layout->stride = meta->stride[0];
		layout->uv_stride = meta->stride[1];
		layout->offset = meta->offset[0];
		layout->uv_offset = meta->offset[1];
	}
	if (layout->stride <= 0 || layout->uv_stride <= 0 ||
	    layout->stride < (gint)layout->width ||
	    layout->uv_stride < (gint)layout->width ||
	    layout->stride > UINT16_MAX || layout->uv_stride > UINT16_MAX ||
	    layout->stride != layout->uv_stride || layout->offset != 0 ||
	    layout->uv_offset < layout->offset ||
	    (layout->uv_offset - layout->offset) % (gsize)layout->stride != 0)
		return false;

	hstride = (uint64_t)((layout->uv_offset - layout->offset) /
				     (gsize)layout->stride);
	if (hstride < layout->height || hstride > UINT16_MAX ||
	    hstride > UINT64_MAX / (uint64_t)(guint)layout->stride)
		return false;
	if (hstride * (uint64_t)(guint)layout->stride != layout->uv_offset)
		return false;
	uv_size = (uint64_t)(guint)layout->uv_stride * (layout->height / 2U);
	if ((uint64_t)layout->uv_offset > UINT64_MAX - uv_size)
		return false;
	required_size = (uint64_t)layout->uv_offset + uv_size;
	if (required_size > G_MAXSIZE)
		return false;
	visible_size = gst_memory_get_sizes(memory, &memory_offset,
							 &memory_maxsize);
	if (memory_offset != 0 || visible_size < (gsize)required_size ||
	    gst_buffer_get_size(buffer) < (gsize)required_size)
		return false;
	layout->hstride = (guint)hstride;
	layout->fd = gst_dmabuf_memory_get_fd(memory);
	return layout->fd >= 0;
}

static uint32_t clamp_coord(int value)
{
	if (value < 0)
		return 0;
	if (value > (int)ME_ANALYTICS_COORD_SCALE)
		return ME_ANALYTICS_COORD_SCALE;
	return (uint32_t)value;
}

static bool map_track_state(RockIvaObjectState state, MeTrackState *mapped)
{
	if (!mapped)
		return false;
	switch (state) {
	case ROCKIVA_OBJECT_STATE_FIRST:
		*mapped = ME_TRACK_STATE_NEW;
		return true;
	case ROCKIVA_OBJECT_STATE_TRACKING:
		*mapped = ME_TRACK_STATE_ACTIVE;
		return true;
	case ROCKIVA_OBJECT_STATE_LOST:
		*mapped = ME_TRACK_STATE_LOST;
		return true;
	case ROCKIVA_OBJECT_STATE_DISPEAR:
		*mapped = ME_TRACK_STATE_REMOVED;
		return true;
	default:
		return false;
	}
}

static void detect_callback(const RockIvaDetectResult *result,
						const RockIvaExecuteStatus status, void *userdata)
{
	MeRockivaRunner *runner = userdata;
	MeRockivaFrame *frame = NULL;
	MeNormalizedObservation observation;
	GstClockTime pts = GST_CLOCK_TIME_NONE;
	guint width;
	guint height;
	uint64_t stream_epoch;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];
	char backend_version[ME_ANALYTICS_VERSION_MAX];
	uint32_t i;

	if (!runner || status != ROCKIVA_SUCCESS || !result) {
		if (runner)
			me_log(ME_LOG_WARN, "RockIVA detection callback failed: status=%d",
			       status);
		return;
	}
	memset(channel_id, 0, sizeof(channel_id));
	memset(backend_version, 0, sizeof(backend_version));
	g_mutex_lock(&runner->lock);
	frame = g_hash_table_lookup(runner->frames,
							GUINT_TO_POINTER(result->frameId));
	if (frame) {
		frame->detection_seen = true;
			pts = frame->pts;
			width = frame->width;
			height = frame->height;
			stream_epoch = frame->stream_epoch;
			snprintf(channel_id, sizeof(channel_id), "%s", frame->channel_id);
	} else {
		width = (guint)runner->config.width;
		height = (guint)runner->config.height;
		stream_epoch = runner->stream_epoch;
		snprintf(channel_id, sizeof(channel_id), "%s", runner->channel_id);
	}
	snprintf(backend_version, sizeof(backend_version), "%s",
			 runner->backend_version[0] ? runner->backend_version : "unknown");
	g_mutex_unlock(&runner->lock);

	if (!channel_id[0] || stream_epoch == 0 || result->frameId == 0)
		return;
	me_observation_init(&observation);
	snprintf(observation.channel_id, sizeof(observation.channel_id), "%s",
			 channel_id);
	observation.stream_epoch = stream_epoch;
	observation.frame_id = result->frameId;
	observation.frame_width = width;
	observation.frame_height = height;
	snprintf(observation.backend_name, sizeof(observation.backend_name),
			 "rockiva");
	snprintf(observation.backend_version, sizeof(observation.backend_version),
			 "%s", backend_version);
	snprintf(observation.model_version, sizeof(observation.model_version), "%s",
			 runner->model_version[0] ? runner->model_version : "unknown");
	if (pts != GST_CLOCK_TIME_NONE && pts <= (GstClockTime)INT64_MAX) {
		observation.source_pts = (int64_t)pts;
		observation.source_pts_valid = true;
	}

	for (i = 0; i < result->objNum && i < ROCKIVA_MAX_OBJ_NUM; i++) {
		const RockIvaObjectInfo *object = &result->objInfo[i];
		MeTrackObservation *track;
		MeTrackState track_state;
		uint32_t left;
		uint32_t top;
		uint32_t right;
		uint32_t bottom;

		if (object->type != ROCKIVA_OBJECT_TYPE_PERSON || object->objId == 0 ||
		    !map_track_state(object->state, &track_state) ||
		    observation.track_count >= ME_ANALYTICS_MAX_TRACKS)
			continue;
		left = clamp_coord(object->rect.topLeft.x);
		top = clamp_coord(object->rect.topLeft.y);
		right = clamp_coord(object->rect.bottomRight.x);
		bottom = clamp_coord(object->rect.bottomRight.y);
		if (left >= right || top >= bottom)
			continue;
		track = &observation.tracks[observation.track_count++];
		track->track_id = object->objId;
		track->class_id = ME_TRACK_CLASS_PERSON;
		track->state = track_state;
		track->bbox.left = left;
		track->bbox.top = top;
		track->bbox.right = right;
		track->bbox.bottom = bottom;
		track->score_q = object->score >= 100 ? ME_ANALYTICS_COORD_SCALE
											  : object->score * 100U;
	}

	if (runner->observation_cb)
		runner->observation_cb(runner->observation_userdata, &observation);
}

static void release_callback(const RockIvaReleaseFrames *frames, void *userdata)
{
	MeRockivaRunner *runner = userdata;
	uint32_t count;
	uint32_t i;

	if (!runner || !frames)
		return;
	count = frames->count > ROCKIVA_MAX_OBJ_NUM ? ROCKIVA_MAX_OBJ_NUM
														: frames->count;
	if (frames->count > ROCKIVA_MAX_OBJ_NUM)
		me_log(ME_LOG_WARN, "RockIVA release callback returned %u frames",
		       frames->count);
	for (i = 0; i < count; i++) {
		const RockIvaImage *released = &frames->frames[i];
		MeRockivaFrame *frame;
		GstBuffer *buffer = NULL;

		g_mutex_lock(&runner->lock);
		frame = g_hash_table_lookup(runner->frames,
							GUINT_TO_POINTER(released->frameId));
		if (!frame || frame->fd != released->dataFd ||
				released->channelId != ME_ROCKIVA_CHANNEL_ID ||
				released->dataAddr != NULL || released->dataPhyAddr != NULL) {
			g_mutex_unlock(&runner->lock);
			me_log(ME_LOG_WARN,
			       "RockIVA release mismatch frame=%u fd=%d channel=%u",
			       released->frameId, released->dataFd, released->channelId);
			continue;
		}
		frame->release_seen = true;
		buffer = frame->buffer;
		frame->buffer = NULL;
		g_hash_table_steal(runner->frames,
							 GUINT_TO_POINTER(released->frameId));
		if (runner->pending_frames > 0)
			runner->pending_frames--;
		g_free(frame);
		g_cond_broadcast(&runner->callbacks_cond);
		g_mutex_unlock(&runner->lock);
		if (buffer)
			gst_buffer_unref(buffer);
	}
}

static bool next_frame_id_locked(MeRockivaRunner *runner, uint32_t *out)
{
	uint32_t candidate;
	uint32_t attempts = 0;

	if (!runner || !out)
		return false;
	while (attempts++ < UINT32_MAX) {
		candidate = ++runner->next_frame_id;
		if (candidate != 0 &&
		    !g_hash_table_contains(runner->frames, GUINT_TO_POINTER(candidate))) {
			*out = candidate;
			return true;
		}
	}
	return false;
}

static GstFlowReturn new_sample_callback(GstAppSink *sink, gpointer userdata)
{
	MeRockivaRunner *runner = userdata;
	GstSample *sample = NULL;
	GstBuffer *buffer;
	GstBuffer *drop_buffer = NULL;
	MeRockivaLayout layout;
	MeRockivaFrame *frame;
	RockIvaImage image;
	RockIvaRetCode ret;
	uint32_t frame_id;
	uint64_t stream_epoch;
	char channel_id[ME_ANALYTICS_CHANNEL_ID_MAX];

	if (!runner)
		return GST_FLOW_ERROR;
	g_mutex_lock(&runner->lock);
	if (runner->stopping) {
		g_mutex_unlock(&runner->lock);
		return GST_FLOW_FLUSHING;
	}
	runner->active_callbacks++;
	g_mutex_unlock(&runner->lock);

	sample = gst_app_sink_pull_sample(sink);
	if (!sample)
		goto done;
	if (!inspect_sample(sample, &runner->config, &layout)) {
		g_mutex_lock(&runner->lock);
		runner->rejected_samples++;
		if (runner->rejected_samples == 1) {
			GstBuffer *rejected_buffer = gst_sample_get_buffer(sample);
			GstCaps *rejected_caps = gst_sample_get_caps(sample);
			GstMemory *rejected_memory = rejected_buffer &&
				gst_buffer_n_memory(rejected_buffer) == 1
				? gst_buffer_peek_memory(rejected_buffer, 0)
				: NULL;
			gchar *caps_text = rejected_caps
				? gst_caps_to_string(rejected_caps)
				: NULL;
			me_log(ME_LOG_WARN,
			       "RockIVA sample rejected: caps=%s memories=%u dmabuf=%s size=%" G_GSIZE_FORMAT,
			       caps_text ? caps_text : "(none)",
			       rejected_buffer ? gst_buffer_n_memory(rejected_buffer) : 0,
			       rejected_memory && gst_is_dmabuf_memory(rejected_memory) ? "yes" : "no",
			       rejected_buffer ? gst_buffer_get_size(rejected_buffer) : 0);
			g_free(caps_text);
		}
		g_mutex_unlock(&runner->lock);
		goto done;
	}
	buffer = gst_sample_get_buffer(sample);
	if (!buffer)
		goto done;

	memset(&image, 0, sizeof(image));
	g_mutex_lock(&runner->lock);
	if (runner->stopping || !next_frame_id_locked(runner, &frame_id)) {
		g_mutex_unlock(&runner->lock);
		goto done;
	}
	stream_epoch = runner->stream_epoch;
	snprintf(channel_id, sizeof(channel_id), "%s", runner->channel_id);
	frame = g_new0(MeRockivaFrame, 1);
	frame->buffer = gst_buffer_ref(buffer);
	frame->fd = layout.fd;
	frame->pts = GST_BUFFER_PTS(buffer);
	frame->width = layout.width;
	frame->height = layout.height;
	frame->hstride = layout.hstride;
	frame->stride = layout.stride;
	frame->stream_epoch = stream_epoch;
	snprintf(frame->channel_id, sizeof(frame->channel_id), "%s", channel_id);
	g_hash_table_insert(runner->frames, GUINT_TO_POINTER(frame_id), frame);
	runner->pending_frames++;
	g_mutex_unlock(&runner->lock);

	image.frameId = frame_id;
	image.channelId = ME_ROCKIVA_CHANNEL_ID;
	image.info.width = (uint16_t)layout.width;
	image.info.height = (uint16_t)layout.height;
	image.info.wstride = (uint16_t)layout.stride;
	image.info.hstride = (uint16_t)layout.hstride;
	image.info.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
	image.info.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
	image.dataFd = layout.fd;
	ret = ROCKIVA_PushFrame(runner->handle, &image, NULL);

	g_mutex_lock(&runner->lock);
	frame = g_hash_table_lookup(runner->frames, GUINT_TO_POINTER(frame_id));
	if (ret != ROCKIVA_RET_SUCCESS && frame && !frame->detection_seen &&
	    !frame->release_seen) {
		g_hash_table_steal(runner->frames, GUINT_TO_POINTER(frame_id));
		if (runner->pending_frames > 0)
			runner->pending_frames--;
		drop_buffer = frame->buffer;
		frame->buffer = NULL;
		g_free(frame);
		g_cond_broadcast(&runner->callbacks_cond);
	}
	g_mutex_unlock(&runner->lock);
	if (ret != ROCKIVA_RET_SUCCESS) {
		me_log(ME_LOG_WARN, "RockIVA PushFrame failed: ret=%d (%s)", ret,
		       ret_name(ret));
		if (drop_buffer)
			gst_buffer_unref(drop_buffer);
	}

done:
	if (sample)
		gst_sample_unref(sample);
	g_mutex_lock(&runner->lock);
	if (runner->active_callbacks > 0)
		runner->active_callbacks--;
	g_cond_broadcast(&runner->callbacks_cond);
	g_mutex_unlock(&runner->lock);
	return GST_FLOW_OK;
}

static bool wait_for_active_callbacks(MeRockivaRunner *runner, gint64 deadline)
{
	bool complete;

	g_mutex_lock(&runner->lock);
	while (runner->active_callbacks > 0) {
		if (!g_cond_wait_until(&runner->callbacks_cond, &runner->lock,
							   deadline))
			break;
	}
	complete = runner->active_callbacks == 0;
	g_mutex_unlock(&runner->lock);
	return complete;
}

static bool wait_for_pending_frames(MeRockivaRunner *runner, gint64 deadline)
{
	bool complete;

	g_mutex_lock(&runner->lock);
	while (runner->pending_frames > 0) {
		if (!g_cond_wait_until(&runner->callbacks_cond, &runner->lock,
							   deadline))
			break;
	}
	complete = runner->pending_frames == 0;
	g_mutex_unlock(&runner->lock);
	return complete;
}

static void destroy_partial_runner(MeRockivaRunner *runner)
{
	if (!runner)
		return;
	if (runner->detect_initialized)
		(void)ROCKIVA_DETECT_Release(runner->handle);
	if (runner->handle_initialized)
		(void)ROCKIVA_Release(runner->handle);
	if (runner->appsink)
		gst_object_unref(runner->appsink);
	if (runner->frames)
		g_hash_table_destroy(runner->frames);
	g_cond_clear(&runner->callbacks_cond);
	g_mutex_clear(&runner->lock);
	g_free(runner);
}

MeRockivaRunner *me_rockiva_runner_new(
	GstAppSink *appsink, const MeAnalyticsConfig *config,
	MeRockivaObservationCb observation_cb, void *observation_userdata,
	char *err, size_t errsz)
{
	MeRockivaRunner *runner;
	RockIvaInitParam init_params;
	RockIvaDetTaskParams det_params;
	RockIvaDetModel model;
	RockIvaRetCode ret;
	GstAppSinkCallbacks callbacks = {0};
	char version[ME_ROCKIVA_VERSION_MAX] = {0};

	if (!appsink || !config || !observation_cb) {
		rockiva_set_err(err, errsz, "RockIVA appsink/config/callback required");
		return NULL;
	}
	if (!model_from_name(config->model, &model)) {
		if (err && errsz > 0)
			snprintf(err, errsz, "unsupported RockIVA model: %s",
				 config->model);
		return NULL;
	}
	if (config->width > UINT16_MAX || config->height > UINT16_MAX) {
		rockiva_set_err(err, errsz, "RockIVA analytics dimensions exceed uint16_t");
		return NULL;
	}

	runner = g_new0(MeRockivaRunner, 1);
	g_mutex_init(&runner->lock);
	g_cond_init(&runner->callbacks_cond);
	runner->appsink = GST_APP_SINK(gst_object_ref(appsink));
	runner->frames = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL,
										   g_free);
	runner->next_frame_id = 0;
	runner->stream_epoch = 1;
	snprintf(runner->channel_id, sizeof(runner->channel_id), "camera");
	runner->config = *config;
	runner->observation_cb = observation_cb;
	runner->observation_userdata = observation_userdata;
	g_strlcpy(runner->model_version, config->model,
			  sizeof(runner->model_version));

	memset(&init_params, 0, sizeof(init_params));
	init_params.logLevel = ROCKIVA_LOG_ERROR;
	init_params.coreMask = ME_ROCKIVA_CORE_MASK;
	init_params.newModelInstance = 1;
	init_params.channelId = ME_ROCKIVA_CHANNEL_ID;
	init_params.detModel = model;
	init_params.detObjectType =
		ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
	snprintf(init_params.modelPath, sizeof(init_params.modelPath), "%s",
			 ME_ROCKIVA_MODEL_PATH);
	init_params.imageInfo.width = (uint16_t)config->width;
	init_params.imageInfo.height = (uint16_t)config->height;
	init_params.imageInfo.wstride = (uint16_t)config->width;
	init_params.imageInfo.hstride = (uint16_t)config->height;
	init_params.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
	init_params.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;

	ret = ROCKIVA_GetVersion(ME_ROCKIVA_VERSION_MAX, version);
	if (ret == ROCKIVA_RET_SUCCESS && version[0])
		snprintf(runner->backend_version, sizeof(runner->backend_version), "%s",
			 version);
	else
		snprintf(runner->backend_version, sizeof(runner->backend_version),
			 "unknown");

	ret = ROCKIVA_Init(&runner->handle, ROCKIVA_MODE_VIDEO, &init_params, runner);
	if (ret != ROCKIVA_RET_SUCCESS) {
		if (err && errsz > 0)
			snprintf(err, errsz, "ROCKIVA_Init failed: %d", ret);
		destroy_partial_runner(runner);
		return NULL;
	}
	runner->handle_initialized = true;

	memset(&det_params, 0, sizeof(det_params));
	det_params.detObjectType =
		ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
	if (config->score_threshold_q > 0) {
		uint32_t threshold = (config->score_threshold_q + 99U) / 100U;
		det_params.scores[ROCKIVA_OBJECT_TYPE_PERSON] =
			(uint8_t)(threshold > 100U ? 100U : threshold);
	}
	ret = ROCKIVA_DETECT_Init(runner->handle, &det_params, detect_callback);
	if (ret != ROCKIVA_RET_SUCCESS) {
		if (err && errsz > 0)
			snprintf(err, errsz, "ROCKIVA_DETECT_Init failed: %d", ret);
		destroy_partial_runner(runner);
		return NULL;
	}
	runner->detect_initialized = true;
	ret = ROCKIVA_SetFrameReleaseCallback(runner->handle, release_callback);
	if (ret != ROCKIVA_RET_SUCCESS) {
		if (err && errsz > 0)
			snprintf(err, errsz, "ROCKIVA_SetFrameReleaseCallback failed: %d",
				 ret);
		destroy_partial_runner(runner);
		return NULL;
	}
	runner->release_callback_initialized = true;

	callbacks.new_sample = new_sample_callback;
	gst_app_sink_set_callbacks(runner->appsink, &callbacks, runner, NULL);
	me_log(ME_LOG_INFO,
	       "RockIVA analytics ready: model=%s input=%dx%d fps=%d version=%s",
	       config->model, config->width, config->height, config->fps,
	       runner->backend_version);
	return runner;
}

void me_rockiva_runner_set_stream(MeRockivaRunner *runner,
							  const char *channel_id, uint64_t stream_epoch)
{
	if (!runner)
		return;
	g_mutex_lock(&runner->lock);
	snprintf(runner->channel_id, sizeof(runner->channel_id), "%s",
			 channel_id && channel_id[0] ? channel_id : "camera");
	runner->stream_epoch = stream_epoch ? stream_epoch : 1;
	g_mutex_unlock(&runner->lock);
}

int me_rockiva_runner_free(MeRockivaRunner *runner)
{
	RockIvaRetCode ret;
	gint64 deadline;
	bool safe;
	GstAppSinkCallbacks callbacks = { 0 };

	if (!runner)
		return 0;
	g_mutex_lock(&runner->lock);
	runner->stopping = true;
	g_mutex_unlock(&runner->lock);
	/* gst_app_sink_set_callbacks requires a non-NULL callback table; an empty
	 * table disables the streaming callback without triggering a critical. */
	gst_app_sink_set_callbacks(runner->appsink, &callbacks, NULL, NULL);
	deadline = g_get_monotonic_time() +
			(gint64)ME_ROCKIVA_WAIT_TIMEOUT_MS * G_TIME_SPAN_MILLISECOND;
	safe = wait_for_active_callbacks(runner, deadline);
	if (runner->handle_initialized) {
		ret = ROCKIVA_WaitFinish(runner->handle, -1,
							ME_ROCKIVA_WAIT_TIMEOUT_MS);
		if (ret != ROCKIVA_RET_SUCCESS && ret != ROCKIVA_RET_UNSUPPORTED) {
			me_log(ME_LOG_ERROR, "ROCKIVA_WaitFinish failed: %d", ret);
			safe = false;
		}
		if (ret == ROCKIVA_RET_UNSUPPORTED) {
			deadline = g_get_monotonic_time() +
					(gint64)ME_ROCKIVA_WAIT_TIMEOUT_MS *
					G_TIME_SPAN_MILLISECOND;
		}
		if (ret == ROCKIVA_RET_SUCCESS || ret == ROCKIVA_RET_UNSUPPORTED) {
			deadline = g_get_monotonic_time() +
					(gint64)ME_ROCKIVA_WAIT_TIMEOUT_MS *
					G_TIME_SPAN_MILLISECOND;
			safe = wait_for_pending_frames(runner, deadline) && safe;
		}
		if (!safe) {
			me_log(ME_LOG_ERROR,
			       "RockIVA shutdown left %u frames pending; keeping SDK state alive",
			       runner->pending_frames);
			return -1;
		}
		if (runner->detect_initialized) {
			ret = ROCKIVA_DETECT_Release(runner->handle);
			if (ret != ROCKIVA_RET_SUCCESS) {
				me_log(ME_LOG_ERROR, "ROCKIVA_DETECT_Release failed: %d", ret);
				return -1;
			}
			runner->detect_initialized = false;
		}
		ret = ROCKIVA_WaitFinish(runner->handle, -1,
							ME_ROCKIVA_WAIT_TIMEOUT_MS);
		if (ret != ROCKIVA_RET_SUCCESS && ret != ROCKIVA_RET_UNSUPPORTED) {
			me_log(ME_LOG_ERROR, "ROCKIVA final WaitFinish failed: %d", ret);
			return -1;
		}
		ret = ROCKIVA_Release(runner->handle);
		if (ret != ROCKIVA_RET_SUCCESS) {
			me_log(ME_LOG_ERROR, "ROCKIVA_Release failed: %d", ret);
			return -1;
		}
		runner->handle_initialized = false;
	}
	if (runner->appsink)
		gst_object_unref(runner->appsink);
	g_hash_table_destroy(runner->frames);
	g_cond_clear(&runner->callbacks_cond);
	g_mutex_clear(&runner->lock);
	g_free(runner);
	return 0;
}
