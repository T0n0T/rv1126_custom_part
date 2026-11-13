#define _POSIX_C_SOURCE 200809L
#include "flow_camera.h"

#include <rk_aiq_user_api2_imgproc.h>
#include <rk_aiq_user_api2_sysctl.h>
#include <rk_mpi_vi.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <gst/allocators/gstdmabuf.h>
#include "flow_dma.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARMUP_FRAMES 15U

struct capture_tag {
	uint64_t index;
	gint64 arrival_us;
};

struct flow_camera {
	struct flow_camera_options options;
	rk_aiq_sys_ctx_t *aiq;
	int aiq_started;
	GstElement *capture;
	GstElement *capture_sink;
	GstElement *display;
	GstElement *display_source;
	GstElement *display_sink;
	GQuark tag_key;
	GMutex lock;
	GCond frame_ready;
	GThread *preview_thread;
	GstSample *latest_sample;
	GstBufferPool *display_pool;
	struct flow_overlay overlay;
	int stopping;
	int preview_failed;
	int rga_ready;
	uint64_t preview_frames;
	uint64_t dma_input_frames;
	uint64_t dma_output_frames;
	gint64 first_preview_us;
	gint64 last_preview_us;
	double total_preview_ms;
	FILE *stats;
	uint64_t captured;
	uint64_t processed;
	uint64_t warmup;
	uint64_t last_index;
	uint64_t skipped;
	gint64 sample_arrival_us;
	gint64 first_processed_us;
	gint64 last_processed_us;
	double preprocess_ms;
	double total_preprocess_ms;
	double total_infer_ms;
	double total_post_ms;
	double total_age_ms;
	int snapshot_saved;
	int source_snapshot_saved;
};

static int pipeline_error(GstElement *pipeline)
{
	GstBus *bus;
	GstMessage *message;
	int status = 0;
	if (!pipeline) return 0;
	bus = gst_element_get_bus(pipeline);
	while ((message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
		if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
			GError *error = NULL;
			gchar *debug = NULL;
			gst_message_parse_error(message, &error, &debug);
			fprintf(stderr, "GStreamer %s: %s (%s)\n", GST_OBJECT_NAME(message->src),
				error ? error->message : "unknown error", debug ? debug : "");
			g_clear_error(&error);
			g_free(debug);
		} else {
			fprintf(stderr, "unexpected end of live camera/display stream\n");
		}
		status = -1;
		gst_message_unref(message);
	}
	gst_object_unref(bus);
	return status;
}

static GstPadProbeReturn stamp_capture(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
	struct flow_camera *camera = data;
	GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
	struct capture_tag *tag = g_new(struct capture_tag, 1);
	(void)pad;
	g_mutex_lock(&camera->lock);
	tag->index = camera->captured++;
	g_mutex_unlock(&camera->lock);
	tag->arrival_us = g_get_monotonic_time();
	/* Attach bookkeeping without changing the source's PTS or pixel data. */
	gst_mini_object_set_qdata(GST_MINI_OBJECT(buffer), camera->tag_key, tag, g_free);
	return GST_PAD_PROBE_OK;
}

static int start_aiq(struct flow_camera *camera)
{
	rk_aiq_static_info_t sensor = {0};
	XCamReturn ret;
	const struct flow_camera_options *o = &camera->options;
	if (rk_aiq_uapi2_sysctl_enumStaticMetasByPhyId(o->camera_id, &sensor) != XCAM_RETURN_NO_ERROR ||
	    sensor.sensor_info.phyId == -1 || !sensor.sensor_info.sensor_name[0]) {
		fprintf(stderr, "cannot find camera %d\n", o->camera_id);
		return -1;
	}
	fprintf(stderr, "camera=%d sensor=%s iq_dir=%s\n", o->camera_id,
		sensor.sensor_info.sensor_name, o->iq_dir);
	/* Match media_engine/src/aiq/aiq_ctrl.c: normal/day, normal mode, auto AF. */
	ret = rk_aiq_uapi2_sysctl_preInit_scene(sensor.sensor_info.sensor_name, "normal", "day");
	if (ret != XCAM_RETURN_NO_ERROR)
		fprintf(stderr, "aiq preInit_scene failed: %d\n", ret);
	camera->aiq = rk_aiq_uapi2_sysctl_init(sensor.sensor_info.sensor_name, o->iq_dir, NULL, NULL);
	if (!camera->aiq || rk_aiq_uapi2_sysctl_prepare(camera->aiq, 0, 0,
						     RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR)
		return -1;
	/* Match the SDK's raw-pipe ownership contract before starting the sensor. */
	if (RK_MPI_VI_CtrlRawStream(rk_aiq_uapi2_judgeNeedRawPipeCtrl(camera->aiq) ? RK_FALSE : RK_TRUE) != 0)
		return -1;
	if (rk_aiq_uapi2_sysctl_start(camera->aiq) != XCAM_RETURN_NO_ERROR)
		return -1;
	camera->aiq_started = 1;
	ret = rk_aiq_uapi2_setFocusMode(camera->aiq, OP_AUTO);
	if (ret != XCAM_RETURN_NO_ERROR) {
		fprintf(stderr, "aiq setFocusMode auto failed: %d\n", ret);
		return -1;
	}
	fprintf(stderr, "AIQ_READY scene=normal/day mode=normal af=auto (media_engine initialization contract)\n");
	return 0;
}

static int start_capture(struct flow_camera *camera)
{
	const struct flow_camera_options *o = &camera->options;
	GstElement *source, *filter;
	GstCaps *caps;
	GstPad *pad;
	camera->capture = gst_pipeline_new("flow-camera-capture");
	source = gst_element_factory_make("v4l2src", "camera");
	filter = gst_element_factory_make("capsfilter", "camera-format");
	camera->capture_sink = gst_element_factory_make("appsink", "latest-frame");
	if (!camera->capture || !source || !filter || !camera->capture_sink) {
		if (source) gst_object_unref(source);
		if (filter) gst_object_unref(filter);
		if (camera->capture_sink) gst_object_unref(camera->capture_sink);
		camera->capture_sink = NULL;
		return -1;
	}
	gst_bin_add_many(GST_BIN(camera->capture), source, filter, camera->capture_sink, NULL);
	g_object_set(source, "device", o->device, "io-mode", 4 /* DMA-BUF export */, NULL);
	caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB",
		"width", G_TYPE_INT, o->width, "height", G_TYPE_INT, o->height,
		"framerate", GST_TYPE_FRACTION, o->fps, 1, NULL);
	g_object_set(filter, "caps", caps, NULL);
	gst_caps_unref(caps);
	g_object_set(camera->capture_sink, "sync", FALSE, "async", FALSE,
		"max-buffers", 1U, "drop", TRUE, "wait-on-eos", FALSE,
		"enable-last-sample", FALSE, NULL);
	pad = gst_element_get_static_pad(camera->capture_sink, "sink");
	gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, stamp_capture, camera, NULL);
	gst_object_unref(pad);
	if (!gst_element_link_many(source, filter, camera->capture_sink, NULL) ||
	    gst_element_set_state(camera->capture, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		return -1;
	fprintf(stderr, "capture=%s RGB24 DMA-BUF bytes=%s %dx%d@%d rotation=%d latest-frame queue=1\n",
		o->device, o->input_bgr ? "BGR" : "RGB", o->width, o->height, o->fps, o->rotation);
	return 0;
}

static int start_display(struct flow_camera *camera)
{
	const struct flow_camera_options *o = &camera->options;
	GstCaps *caps;
	camera->display = gst_pipeline_new("flow-camera-display");
	camera->display_source = gst_element_factory_make("appsrc", "annotated-frame");
	camera->display_sink = gst_element_factory_make("kmssink", "screen");
	if (!camera->display || !camera->display_source || !camera->display_sink) {
		if (camera->display_source) gst_object_unref(camera->display_source);
		if (camera->display_sink) gst_object_unref(camera->display_sink);
		camera->display_source = NULL;
		camera->display_sink = NULL;
		return -1;
	}
	gst_bin_add_many(GST_BIN(camera->display), camera->display_source, camera->display_sink, NULL);
	caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "BGRx",
		"width", G_TYPE_INT, o->screen_width, "height", G_TYPE_INT, o->screen_height,
		"framerate", GST_TYPE_FRACTION, 0, 1, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);
	gst_app_src_set_caps(GST_APP_SRC(camera->display_source), caps);
	g_object_set(camera->display_source, "is-live", TRUE, "format", GST_FORMAT_TIME,
		"max-buffers", (guint64)1, "max-bytes", (guint64)0,
		"leaky-type", 2 /* downstream */, "block", FALSE, NULL);
	g_object_set(camera->display_sink, "connector-id", o->connector_id, "plane-id", o->plane_id,
		"sync", FALSE, "async", FALSE, "skip-vsync", TRUE,
		"enable-last-sample", FALSE, NULL);
	camera->display_pool = flow_dma_pool(caps, o->screen_width, o->screen_height);
	gst_caps_unref(caps);
	if (!camera->display_pool || !gst_element_link(camera->display_source, camera->display_sink) ||
	    gst_element_set_state(camera->display, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
		return -1;
	fprintf(stderr, "display=KMS connector=%d plane=%d size=%dx%d BGRx screen_rotation=%d logical=%dx%d\n",
		o->connector_id, o->plane_id, o->screen_width, o->screen_height, o->screen_rotation,
		o->screen_rotation % 180 ? o->screen_height : o->screen_width,
		o->screen_rotation % 180 ? o->screen_width : o->screen_height);
	return 0;
}

static gpointer preview_loop(gpointer data)
{
	struct flow_camera *camera = data;
	const struct flow_camera_options *o = &camera->options;
	gint64 deadline = g_get_monotonic_time() + 10000000;
	for (;;) {
		g_mutex_lock(&camera->lock);
		int stopping = camera->stopping;
		g_mutex_unlock(&camera->lock);
		if (stopping) break;
		if (pipeline_error(camera->capture) || pipeline_error(camera->display)) goto failed;
		GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(camera->capture_sink), 250 * GST_MSECOND);
		if (!sample) {
			if (g_get_monotonic_time() < deadline) continue;
			fprintf(stderr, "camera produced no preview frame for 10 seconds\n");
			goto failed;
		}
		deadline = g_get_monotonic_time() + 10000000;
		GstBuffer *input = gst_sample_get_buffer(sample);
		const struct capture_tag *tag = gst_mini_object_get_qdata(GST_MINI_OBJECT(input), camera->tag_key);
		if (!tag || !GST_BUFFER_PTS_IS_VALID(input)) { gst_sample_unref(sample); goto failed; }
		g_mutex_lock(&camera->lock);
		if (camera->warmup < WARMUP_FRAMES) {
			++camera->warmup;
			g_mutex_unlock(&camera->lock);
			gst_sample_unref(sample);
			continue;
		}
		if (camera->latest_sample) gst_sample_unref(camera->latest_sample);
		camera->latest_sample = gst_sample_ref(sample);
		struct flow_overlay overlay = camera->overlay;
		int need_snapshot = o->snapshot_path && !camera->snapshot_saved && camera->processed > 0;
		g_cond_broadcast(&camera->frame_ready);
		g_mutex_unlock(&camera->lock);
		if (gst_buffer_n_memory(input) == 1 && gst_is_dmabuf_memory(gst_buffer_peek_memory(input, 0)))
			++camera->dma_input_frames;
		if (camera->display) {
			gint64 start = g_get_monotonic_time();
			GstBuffer *output = NULL;
			GstBufferPoolAcquireParams acquire = {.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_DONTWAIT};
			if (gst_buffer_pool_acquire_buffer(camera->display_pool, &output, &acquire) != GST_FLOW_OK) {
				/* Do not pin capture or shutdown behind a slow display consumer. */
				gst_sample_unref(sample); continue;
			}
			/* One RGA operation combines camera and whole-panel orientation. */
			if (flow_dma_blit(sample, output, (o->rotation + o->screen_rotation) % 360,
					o->input_bgr, o->screen_width, o->screen_height) < 0 ||
			    flow_dma_cpu_sync(output, 1) != 0) {
				fprintf(stderr, "RGA preview blit/cache synchronization failed\n");
				gst_buffer_unref(output); gst_sample_unref(sample); goto failed;
			}
			GstMapInfo map;
			GstVideoMeta *meta = gst_buffer_get_video_meta(output);
			if (!meta || !gst_buffer_map(output, &map, GST_MAP_WRITE)) {
				(void)flow_dma_cpu_sync(output, 0);
				gst_buffer_unref(output); gst_sample_unref(sample); goto failed;
			}
			int sw = o->rotation % 180 ? o->height : o->width;
			int sh = o->rotation % 180 ? o->width : o->height;
			double fps = camera->preview_frames > 1 ? (camera->preview_frames - 1) * 1000000.0 /
				(camera->last_preview_us - camera->first_preview_us) : 0;
			if (flow_draw_overlay_rotated(map.data, o->screen_width, o->screen_height, meta->stride[0],
				o->screen_rotation, sw, sh, &overlay, fps, GST_BUFFER_PTS(input) / (double)GST_MSECOND) != 0) {
				gst_buffer_unmap(output, &map);
				(void)flow_dma_cpu_sync(output, 0);
				gst_buffer_unref(output); gst_sample_unref(sample); goto failed;
			}
			if (need_snapshot) {
				if (flow_save_ppm(o->snapshot_path, map.data, o->screen_width, o->screen_height) != 0)
					fprintf(stderr, "cannot save preview snapshot: %s\n", strerror(errno));
				else camera->snapshot_saved = 1;
			}
			gst_buffer_unmap(output, &map);
			if (flow_dma_cpu_sync(output, 0) != 0) {
				gst_buffer_unref(output); gst_sample_unref(sample); goto failed;
			}
			GST_BUFFER_PTS(output) = GST_BUFFER_PTS(input);
			GST_BUFFER_DURATION(output) = GST_BUFFER_DURATION(input);
			GST_BUFFER_OFFSET(output) = tag->index;
			if (gst_app_src_push_buffer(GST_APP_SRC(camera->display_source), output) != GST_FLOW_OK) {
				gst_sample_unref(sample); goto failed;
			}
			++camera->dma_output_frames;
			camera->last_preview_us = g_get_monotonic_time();
			if (!camera->preview_frames) camera->first_preview_us = camera->last_preview_us;
			++camera->preview_frames;
			camera->total_preview_ms += (camera->last_preview_us - start) / 1000.0;
		}
		gst_sample_unref(sample);
	}
	return NULL;
failed:
	g_mutex_lock(&camera->lock);
	camera->preview_failed = 1;
	g_cond_broadcast(&camera->frame_ready);
	g_mutex_unlock(&camera->lock);
	return NULL;
}

struct flow_camera *flow_camera_open(const struct flow_camera_options *options)
{
	struct flow_camera *camera = calloc(1, sizeof(*camera));
	if (!camera) return NULL;
	camera->options = *options;
	g_mutex_init(&camera->lock);
	g_cond_init(&camera->frame_ready);
	gst_init(NULL, NULL);
	camera->tag_key = g_quark_from_static_string("flow-camera-frame-index");
	if (options->stats_path && !(camera->stats = fopen(options->stats_path, "wx")))
		goto error;
	if (!options->no_display) {
		if (flow_dma_init() < 0) goto error;
		camera->rga_ready = 1;
	}
	if (start_aiq(camera) != 0 || start_capture(camera) != 0 ||
	    (!options->no_display && start_display(camera) != 0))
		goto error;
	camera->preview_thread = g_thread_new("flow-preview", preview_loop, camera);
	return camera;
error:
	fprintf(stderr, "camera/display startup failed: %s\n", strerror(errno));
	(void)flow_camera_close(camera);
	return NULL;
}

int flow_camera_next(struct flow_camera *camera, uint8_t *input,
		     struct frame_metadata *metadata, const volatile sig_atomic_t *stop)
{
	while (!*stop) {
		GstSample *sample = NULL;
		g_mutex_lock(&camera->lock);
		if (camera->preview_failed) { g_mutex_unlock(&camera->lock); return -1; }
		if (camera->latest_sample) {
			GstBuffer *latest = gst_sample_get_buffer(camera->latest_sample);
			const struct capture_tag *tag = gst_mini_object_get_qdata(GST_MINI_OBJECT(latest), camera->tag_key);
			if (!camera->processed || tag->index > camera->last_index)
				sample = gst_sample_ref(camera->latest_sample);
		}
		if (!sample) g_cond_wait_until(&camera->frame_ready, &camera->lock, g_get_monotonic_time() + 250000);
		g_mutex_unlock(&camera->lock);
		if (!sample) continue;
		GstBuffer *buffer = gst_sample_get_buffer(sample);
		const struct capture_tag *tag = gst_mini_object_get_qdata(GST_MINI_OBJECT(buffer), camera->tag_key);
		GstVideoInfo info;
		GstVideoFrame frame;
		if (flow_dma_cpu_sync(buffer, 1) != 0) { gst_sample_unref(sample); return -1; }
		if (!tag || tag->index > INT64_MAX || !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
		    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGB ||
		    !gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
			(void)flow_dma_cpu_sync(buffer, 0);
			gst_sample_unref(sample); return -1;
		}
		metadata->frame_index = (int64_t)tag->index;
		metadata->pts_ms = GST_BUFFER_PTS(buffer) / (double)GST_MSECOND;
		camera->sample_arrival_us = tag->arrival_us;
		gint64 start = g_get_monotonic_time();
		int (*letterbox)(const uint8_t *, int, int, int, int, uint8_t *, struct frame_metadata *) =
			camera->options.input_bgr ? flow_letterbox_bgr : flow_letterbox_rgb;
		int status = letterbox(GST_VIDEO_FRAME_PLANE_DATA(&frame, 0),
			GST_VIDEO_FRAME_WIDTH(&frame), GST_VIDEO_FRAME_HEIGHT(&frame),
			GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), camera->options.rotation, input, metadata);
		if (status == 0 && camera->options.source_snapshot_path && !camera->source_snapshot_saved) {
			int (*save_snapshot)(const char *, const uint8_t *, int, int, int, int) =
				camera->options.input_bgr ? flow_save_rotated_bgr_ppm : flow_save_rotated_ppm;
			status = save_snapshot(camera->options.source_snapshot_path,
				GST_VIDEO_FRAME_PLANE_DATA(&frame, 0), GST_VIDEO_FRAME_WIDTH(&frame),
				GST_VIDEO_FRAME_HEIGHT(&frame), GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0), camera->options.rotation);
			if (status == 0) camera->source_snapshot_saved = 1;
		}
		gst_video_frame_unmap(&frame);
		if (flow_dma_cpu_sync(buffer, 0) != 0) status = -1;
		gst_sample_unref(sample);
		camera->preprocess_ms = (g_get_monotonic_time() - start) / 1000.0;
		return status == 0 ? 1 : -1;
	}
	return 0;
}

int flow_camera_present(struct flow_camera *camera, const uint8_t *input,
			const struct frame_metadata *metadata,
			const struct flow_detection *detections, size_t count,
			double infer_ms, double post_ms, const struct flow_overlay *event_overlay)
{
	(void)input;
	gint64 now = g_get_monotonic_time();
	g_mutex_lock(&camera->lock);
	if (camera->preview_failed) { g_mutex_unlock(&camera->lock); return -1; }
	if (!camera->processed) {
		camera->first_processed_us = now;
		camera->skipped = (uint64_t)metadata->frame_index - camera->warmup;
	} else camera->skipped += (uint64_t)metadata->frame_index - camera->last_index - 1;
	camera->last_index = (uint64_t)metadata->frame_index;
	camera->last_processed_us = now;
	++camera->processed;
	double fps = camera->processed > 1 ?
		(camera->processed - 1) * 1000000.0 / (now - camera->first_processed_us) : 0;
	if (event_overlay) camera->overlay = *event_overlay;
	else {
		camera->overlay.count = count;
		for (size_t i = 0; i < count; ++i) {
			camera->overlay.tracks[i] = (struct flow_track_view){.detection = detections[i], .track_id = -1};
		}
	}
	camera->overlay.pts_ms = metadata->pts_ms;
	camera->overlay.detector_fps = fps;
	camera->overlay.infer_ms = infer_ms;
	camera->total_preprocess_ms += camera->preprocess_ms;
	camera->total_infer_ms += infer_ms;
	camera->total_post_ms += post_ms;
	double age_ms = (now - camera->sample_arrival_us) / 1000.0;
	camera->total_age_ms += age_ms;
	g_mutex_unlock(&camera->lock);
	if (camera->processed == 1 || camera->processed % 25 == 0)
		fprintf(stderr, "LIVE frame=%" PRIu64 " source=%" PRId64 " pts_ms=%.3f ai_fps=%.2f "
			"skipped=%" PRIu64 " preprocess_ms=%.3f age_to_result_ms=%.3f\n",
			camera->processed, metadata->frame_index, metadata->pts_ms, fps,
			camera->skipped, camera->preprocess_ms, age_ms);
	return 0;
}

int flow_camera_close(struct flow_camera *camera)
{
	uint64_t rendered = 0;
	int status = 0;
	double fps = 0;
	if (!camera) return 0;
	g_mutex_lock(&camera->lock);
	camera->stopping = 1;
	g_cond_broadcast(&camera->frame_ready);
	g_mutex_unlock(&camera->lock);
	if (camera->preview_thread) g_thread_join(camera->preview_thread);
	if (camera->preview_failed) status = -1;
	if (camera->capture) gst_element_set_state(camera->capture, GST_STATE_NULL);
	if (camera->display && camera->preview_frames) {
		GstBus *bus = gst_element_get_bus(camera->display);
		GstMessage *message;
		gst_app_src_end_of_stream(GST_APP_SRC(camera->display_source));
		message = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
		if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_EOS) {
			fprintf(stderr, "display failed to drain cleanly\n");
			status = -1;
		}
		if (message) gst_message_unref(message);
		gst_object_unref(bus);
	}
	if (camera->display_sink) {
		GstStructure *stats = NULL;
		guint64 value = 0;
		g_object_get(camera->display_sink, "stats", &stats, NULL);
		if (stats) {
			if (gst_structure_get_uint64(stats, "rendered", &value)) rendered = value;
			gst_structure_free(stats);
		}
	}
	if (camera->display) gst_element_set_state(camera->display, GST_STATE_NULL);
	if (camera->capture) gst_object_unref(camera->capture);
	if (camera->display) gst_object_unref(camera->display);
	if (camera->latest_sample) gst_sample_unref(camera->latest_sample);
	if (camera->display_pool) {
		gst_buffer_pool_set_active(camera->display_pool, FALSE);
		gst_object_unref(camera->display_pool);
	}
	if (camera->rga_ready) flow_dma_deinit();
	if (camera->aiq) {
		if (camera->aiq_started) rk_aiq_uapi2_sysctl_stop(camera->aiq, false);
		rk_aiq_uapi2_sysctl_deinit(camera->aiq);
	}
	if (camera->processed > 1)
		fps = (camera->processed - 1) * 1000000.0 /
			(camera->last_processed_us - camera->first_processed_us);
	/* Also count frames discarded at shutdown, so all acquired frames reconcile. */
	uint64_t skipped_total = camera->captured - camera->warmup - camera->processed;
	fprintf(stderr, "FLOW_CAMERA_STOP captured=%" PRIu64 " warmup=%" PRIu64
		" processed=%" PRIu64 " skipped=%" PRIu64 " rendered=%" PRIu64 " fps=%.3f\n",
		camera->captured, camera->warmup, camera->processed, skipped_total, rendered, fps);
	if (camera->stats) {
		double n = camera->processed ? (double)camera->processed : 1;
		if (fprintf(camera->stats,
			"{\"captured\":%" PRIu64 ",\"warmup\":%" PRIu64 ",\"processed\":%" PRIu64
			",\"skipped\":%" PRIu64 ",\"rendered\":%" PRIu64 ",\"fps\":%.6f,"
			"\"avg_preprocess_ms\":%.6f,\"avg_infer_ms\":%.6f,\"avg_post_ms\":%.6f,"
			"\"avg_preview_ms\":%.6f,\"avg_arrival_to_result_ms\":%.6f,"
			"\"camera_rotation\":%d,\"screen_rotation\":%d,\"screen_width\":%d,\"screen_height\":%d,"
			"\"capture_caps_format\":\"RGB\",\"capture_color_order\":\"%s\","
			"\"coordinate_space\":\"rotated-camera\","
			"\"preview_frames\":%" PRIu64 ",\"preview_fps\":%.6f,\"dma_input_frames\":%" PRIu64 ",\"dma_output_frames\":%" PRIu64 ","
			"\"display_enabled\":%s,\"clean_display_drain\":%s}\n",
			camera->captured, camera->warmup, camera->processed, skipped_total, rendered, fps,
			camera->total_preprocess_ms / n, camera->total_infer_ms / n, camera->total_post_ms / n,
			camera->total_preview_ms / (camera->preview_frames ? camera->preview_frames : 1), camera->total_age_ms / n, camera->options.rotation,
			camera->options.screen_rotation, camera->options.screen_width, camera->options.screen_height,
			camera->options.input_bgr ? "BGR" : "RGB",
			camera->preview_frames, camera->preview_frames > 1 ?
			(camera->preview_frames - 1) * 1000000.0 / (camera->last_preview_us - camera->first_preview_us) : 0,
			camera->dma_input_frames, camera->dma_output_frames,
			camera->options.no_display ? "false" : "true", status == 0 ? "true" : "false") < 0)
			status = -1;
		if (fclose(camera->stats) != 0) status = -1;
	}
	g_cond_clear(&camera->frame_ready);
	g_mutex_clear(&camera->lock);
	free(camera);
	return status;
}
