/*
 * RockIVA DMA-BUF probe (test tool only).
 *
 * This program owns an independent GStreamer/appsink pipeline.  Its V4L2 mode
 * uses an independent capture node, and its MP4 mode uses decoder output.  It
 * is not part of media_engine and must not be used with the production
 * capture pipeline.
 * RockIVA receives only the DMA-BUF fd; the GStreamer buffer reference kept by
 * this probe is released only by a matching RockIVA frame-release callback.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/video/gstvideopool.h>
#include <gst/video/video.h>
#include <gst/video/gstvideometa.h>
#include <linux/dma-heap.h>

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"
#include "mainpath_guard.h"

#define DEFAULT_CHANNEL 0U
#define DEFAULT_CORE_MASK 0U
#define DEFAULT_MODEL ROCKIVA_DET_MODEL_PFP
#define DEFAULT_TIMEOUT_MS 5000
#define DEFAULT_MIN_PERSON_OBSERVATIONS 0U
#define DEFAULT_MIN_TRACKING_OBSERVATIONS 0U
#define MAX_VERSION 128U
#define DMA_HEAP_UNCACHED "/dev/dma_heap/system-uncached"
#define DMA_HEAP_CACHED "/dev/dma_heap/system"

/* GstV4l2IOMode is private to the v4l2 plugin; its public property enum has
 * been stable since the dmabuf mode was introduced. */
#define GST_V4L2_IO_MODE_DMABUF 4

enum probe_input_kind {
	PROBE_INPUT_V4L2 = 0,
	PROBE_INPUT_MP4,
};

struct probe_options {
	const char *device;
	const char *input_path;
	enum probe_input_kind input_kind;
	const char *model_path;
	unsigned int width;
	unsigned int height;
	unsigned int frames;
	unsigned int fps;
	unsigned int channel_id;
	unsigned int core_mask;
	RockIvaDetModel model;
	int timeout_ms;
	unsigned int min_person_observations;
	unsigned int min_tracking_observations;
	int allow_mainpath;
};

struct frame_layout {
	guint width;
	guint height;
	guint hstride;
	gint stride;
	gint uv_stride;
	gsize offset;
	gsize uv_offset;
	gsize visible_size;
	gsize max_size;
	guint logical_planes;
	guint has_video_meta;
	int32_t fd;
};

enum probe_frame_state {
	PROBE_FRAME_UNUSED = 0,
	PROBE_FRAME_PENDING,
	PROBE_FRAME_ACCEPTED,
	PROBE_FRAME_RELEASED,
	PROBE_FRAME_REJECTED,
};

enum wait_finish_state {
	WAIT_FINISH_NOT_ATTEMPTED = 0,
	WAIT_FINISH_SUCCESS,
	WAIT_FINISH_UNSUPPORTED,
	WAIT_FINISH_FAILED,
};

struct probe_frame {
	GstBuffer *buffer;
	int32_t fd;
	guint width;
	guint height;
	guint hstride;
	gint stride;
	gint uv_stride;
	gsize offset;
	gsize uv_offset;
	gsize visible_size;
	gsize max_size;
	guint logical_planes;
	guint has_video_meta;
	GstClockTime pts;
	uint64_t submitted_ns;
	enum probe_frame_state state;
	int detection_completed;
	int release_seen;
};

struct probe_state {
	pthread_mutex_t metrics_lock;
	pthread_cond_t callbacks_cond;
	int callbacks_cond_initialized;
	uint32_t channel_id;
	uint64_t samples_received;
	uint64_t samples_rejected;
	uint64_t pushed;
	uint64_t push_failures;
	uint64_t detections;
	uint64_t detection_errors;
	uint64_t detection_frame_errors;
	uint64_t detection_unmatched;
	uint64_t detection_duplicates;
	uint64_t detection_completed_frames;
	uint64_t detection_object_overflows;
	uint64_t person_observations;
	uint64_t person_first_observations;
	uint64_t person_tracking_observations;
	uint64_t person_lost_observations;
	uint64_t person_disappear_observations;
	uint64_t release_callbacks;
	uint64_t release_entries;
	uint64_t released_frames;
	uint64_t release_unmatched;
	uint64_t release_duplicates;
	uint64_t release_mismatches;
	uint64_t release_invalid_callbacks;
	uint64_t channel_mismatches;
	struct probe_frame *frames;
	size_t frame_count;
	uint64_t detection_latency_count;
	uint64_t detection_latency_sum_ns;
	uint64_t detection_latency_min_ns;
	uint64_t detection_latency_max_ns;
	uint64_t release_latency_count;
	uint64_t release_latency_sum_ns;
	uint64_t release_latency_min_ns;
	uint64_t release_latency_max_ns;
};

typedef struct _ProbeDmaBufAllocator ProbeDmaBufAllocator;
typedef struct _ProbeDmaBufAllocatorClass ProbeDmaBufAllocatorClass;

struct _ProbeDmaBufAllocator {
	GstDmaBufAllocator parent;
	int heap_fd;
	const char *heap_path;
};

struct _ProbeDmaBufAllocatorClass {
	GstDmaBufAllocatorClass parent_class;
};

#define PROBE_TYPE_DMABUF_ALLOCATOR (probe_dmabuf_allocator_get_type())
#define PROBE_DMABUF_ALLOCATOR(obj) \
	(G_TYPE_CHECK_INSTANCE_CAST((obj), PROBE_TYPE_DMABUF_ALLOCATOR, \
			ProbeDmaBufAllocator))

G_DEFINE_TYPE(ProbeDmaBufAllocator, probe_dmabuf_allocator,
		GST_TYPE_DMABUF_ALLOCATOR)

static GstMemory *probe_dmabuf_alloc(GstAllocator *allocator, gsize size,
					 GstAllocationParams *params)
{
	ProbeDmaBufAllocator *probe_allocator = PROBE_DMABUF_ALLOCATOR(allocator);
	struct dma_heap_allocation_data allocation = { 0 };
	GstMemory *memory;
	gsize total_size;

	if (!params || size > G_MAXSIZE - params->prefix ||
	    size + params->prefix > G_MAXSIZE - params->padding ||
	    probe_allocator->heap_fd < 0)
		return NULL;
	total_size = size + params->prefix + params->padding;
	allocation.len = total_size;
	allocation.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(probe_allocator->heap_fd, DMA_HEAP_IOCTL_ALLOC, &allocation) < 0)
		return NULL;
	memory = gst_dmabuf_allocator_alloc(allocator, (gint)allocation.fd,
			total_size);
	if (!memory) {
		close((int)allocation.fd);
		return NULL;
	}
	memory->align = params->align;
	memory->offset = params->prefix;
	memory->size = size;
	return memory;
}

static void probe_dmabuf_allocator_finalize(GObject *object)
{
	ProbeDmaBufAllocator *allocator = PROBE_DMABUF_ALLOCATOR(object);

	if (allocator->heap_fd >= 0)
		close(allocator->heap_fd);
	G_OBJECT_CLASS(probe_dmabuf_allocator_parent_class)->finalize(object);
}

static void probe_dmabuf_allocator_class_init(ProbeDmaBufAllocatorClass *klass)
{
	GstAllocatorClass *allocator_class = GST_ALLOCATOR_CLASS(klass);
	GObjectClass *object_class = G_OBJECT_CLASS(klass);

	allocator_class->alloc = probe_dmabuf_alloc;
	object_class->finalize = probe_dmabuf_allocator_finalize;
}

static void probe_dmabuf_allocator_init(ProbeDmaBufAllocator *allocator)
{
	allocator->heap_path = DMA_HEAP_UNCACHED;
	allocator->heap_fd = open(DMA_HEAP_UNCACHED, O_RDONLY | O_CLOEXEC);
	if (allocator->heap_fd < 0) {
		allocator->heap_path = DMA_HEAP_CACHED;
		allocator->heap_fd = open(DMA_HEAP_CACHED, O_RDONLY | O_CLOEXEC);
	}
	if (allocator->heap_fd < 0)
		allocator->heap_path = NULL;
}

static GstAllocator *probe_dmabuf_allocator_new(void)
{
	GstAllocator *allocator;

	allocator = g_object_new(PROBE_TYPE_DMABUF_ALLOCATOR, NULL);
	if (!allocator)
		return NULL;
	gst_object_ref_sink(allocator);
	if (PROBE_DMABUF_ALLOCATOR(allocator)->heap_fd < 0) {
		fprintf(stderr, "no usable DMA-BUF heap at %s or %s\n",
			DMA_HEAP_UNCACHED, DMA_HEAP_CACHED);
		gst_object_unref(allocator);
		return NULL;
	}
	return allocator;
}

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static void signal_callbacks_locked(struct probe_state *state)
{
	if (state->callbacks_cond_initialized)
		(void)pthread_cond_broadcast(&state->callbacks_cond);
}

static int callbacks_have_errors_locked(const struct probe_state *state)
{
	return state->detection_errors != 0 || state->detection_frame_errors != 0 ||
	       state->detection_unmatched != 0 || state->detection_duplicates != 0 ||
	       state->release_unmatched != 0 || state->release_duplicates != 0 ||
	       state->release_mismatches != 0 || state->release_invalid_callbacks != 0 ||
	       state->channel_mismatches != 0;
}

static int callbacks_complete_locked(const struct probe_state *state)
{
	return state->detection_completed_frames == state->pushed &&
	       state->released_frames == state->pushed &&
	       !callbacks_have_errors_locked(state);
}

static int make_deadline(struct timespec *deadline, int timeout_ms)
{
	if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
		return -1;
	deadline->tv_sec += timeout_ms / 1000;
	deadline->tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
	if (deadline->tv_nsec >= 1000000000L) {
		deadline->tv_sec++;
		deadline->tv_nsec -= 1000000000L;
	}
	return 0;
}

static int wait_for_callback_completion(struct probe_state *state, int timeout_ms)
{
	struct timespec deadline;
	int wait_result;

	if (!state->callbacks_cond_initialized || make_deadline(&deadline, timeout_ms) != 0) {
		fprintf(stderr, "cannot establish bounded callback wait\n");
		return -1;
	}

	pthread_mutex_lock(&state->metrics_lock);
	for (;;) {
		if (callbacks_complete_locked(state)) {
			printf("callback_completion status=complete pushed=%" PRIu64
			       " detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
			       state->pushed, state->detection_completed_frames,
			       state->released_frames);
			pthread_mutex_unlock(&state->metrics_lock);
			return 0;
		}
		if (callbacks_have_errors_locked(state)) {
			fprintf(stderr, "callback_completion failed: callback accounting error"
				" pushed=%" PRIu64 " detection_completed=%" PRIu64
				" released=%" PRIu64 "\n",
				state->pushed, state->detection_completed_frames,
				state->released_frames);
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
		wait_result = pthread_cond_timedwait(&state->callbacks_cond,
							    &state->metrics_lock, &deadline);
		if (wait_result == ETIMEDOUT) {
			fprintf(stderr, "callback_completion timeout: pushed=%" PRIu64
				" detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
				state->pushed, state->detection_completed_frames,
				state->released_frames);
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
		if (wait_result != 0) {
			fprintf(stderr, "callback_completion wait failed: %s\n",
				strerror(wait_result));
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
	}
}

static const char *model_name(RockIvaDetModel model)
{
	switch (model) {
	case ROCKIVA_DET_MODEL_PFP:
		return "PFP";
	case ROCKIVA_DET_MODEL_CLS8:
		return "CLS8";
	case ROCKIVA_DET_MODEL_PERSON:
		return "PERSON";
	default:
		return "unknown";
	}
}

static int parse_model(const char *value, RockIvaDetModel *model)
{
	if (!strcmp(value, "pfp"))
		*model = ROCKIVA_DET_MODEL_PFP;
	else if (!strcmp(value, "cls8"))
		*model = ROCKIVA_DET_MODEL_CLS8;
	else if (!strcmp(value, "person"))
		*model = ROCKIVA_DET_MODEL_PERSON;
	else
		return -1;
	return 0;
}

static int parse_uint(const char *value, unsigned int *out)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0' || parsed > UINT32_MAX)
		return -1;
	*out = (unsigned int)parsed;
	return 0;
}

static int parse_positive_uint(const char *value, unsigned int *out)
{
	if (parse_uint(value, out) != 0 || *out == 0)
		return -1;
	return 0;
}

static void usage(const char *program)
{
	printf("usage: %s (--device PATH | --input MP4) --model-path DIR --width N --height N "
	       "--frames N --fps N [options]\n", program);
	printf("  --device PATH           independent V4L2 capture device\n");
	printf("  --input MP4             H.264 MP4 decoded to DMA-BUF (test source)\n");
	printf("  --allow-mainpath        explicitly permit /dev/video24 (unsafe)\n");
	printf("  --model-path DIR        RockIVA model directory (required)\n");
	printf("  --width N               requested NV12 width (required)\n");
	printf("  --height N              requested NV12 height (required)\n");
	printf("  --frames N              samples/frames to push (required)\n");
	printf("  --fps N                 requested capture rate (required)\n");
	printf("  --model pfp|cls8|person (default: pfp)\n");
	printf("  --channel N              RockIVA channel ID (default: %u)\n",
	       DEFAULT_CHANNEL);
	printf("  --core-mask MASK         SDK core mask (default: 0x%x)\n",
	       DEFAULT_CORE_MASK);
	printf("  --timeout-ms N           wait/pipeline timeout (default: %d)\n",
	       DEFAULT_TIMEOUT_MS);
	printf("  --min-person N           required person observations (default: %u)\n",
	       DEFAULT_MIN_PERSON_OBSERVATIONS);
	printf("  --min-tracking N         required TRACKING observations (default: %u)\n",
	       DEFAULT_MIN_TRACKING_OBSERVATIONS);
}

static int parse_options(int argc, char **argv, struct probe_options *options)
{
	static const struct option long_options[] = {
		{"device", required_argument, NULL, 'd'},
		{"input", required_argument, NULL, 'u'},
		{"allow-mainpath", no_argument, NULL, 'a'},
		{"model-path", required_argument, NULL, 'm'},
		{"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'h'},
		{"frames", required_argument, NULL, 'n'},
		{"fps", required_argument, NULL, 'f'},
		{"model", required_argument, NULL, 'M'},
		{"channel", required_argument, NULL, 'c'},
		{"core-mask", required_argument, NULL, 'k'},
		{"timeout-ms", required_argument, NULL, 't'},
		{"min-person", required_argument, NULL, 'p'},
		{"min-tracking", required_argument, NULL, 'r'},
		{"help", no_argument, NULL, 'H'},
		{NULL, 0, NULL, 0},
	};
	int option;
	int have_device = 0;
	int have_input = 0;
	int have_width = 0;
	int have_height = 0;
	int have_frames = 0;
	int have_fps = 0;

	memset(options, 0, sizeof(*options));
	options->input_kind = PROBE_INPUT_V4L2;
	options->channel_id = DEFAULT_CHANNEL;
	options->core_mask = DEFAULT_CORE_MASK;
	options->model = DEFAULT_MODEL;
	options->timeout_ms = DEFAULT_TIMEOUT_MS;
	options->min_person_observations = DEFAULT_MIN_PERSON_OBSERVATIONS;
	options->min_tracking_observations = DEFAULT_MIN_TRACKING_OBSERVATIONS;

	while ((option = getopt_long(argc, argv, "du:am:w:h:n:f:M:c:k:t:p:r:H?",
					long_options, NULL)) != -1) {
		switch (option) {
		case 'd':
			options->device = optarg;
			have_device = 1;
			break;
		case 'u':
			options->input_path = optarg;
			options->input_kind = PROBE_INPUT_MP4;
			have_input = 1;
			break;
		case 'a':
			options->allow_mainpath = 1;
			break;
		case 'm':
			options->model_path = optarg;
			break;
		case 'w':
			if (parse_positive_uint(optarg, &options->width) != 0)
				return -1;
			have_width = 1;
			break;
		case 'h':
			if (parse_positive_uint(optarg, &options->height) != 0)
				return -1;
			have_height = 1;
			break;
		case 'n':
			if (parse_positive_uint(optarg, &options->frames) != 0)
				return -1;
			have_frames = 1;
			break;
		case 'f':
			if (parse_positive_uint(optarg, &options->fps) != 0)
				return -1;
			have_fps = 1;
			break;
		case 'M':
			if (parse_model(optarg, &options->model) != 0)
				return -1;
			break;
		case 'c':
			if (parse_uint(optarg, &options->channel_id) != 0)
				return -1;
			break;
		case 'k':
			if (parse_uint(optarg, &options->core_mask) != 0)
				return -1;
			break;
		case 't': {
			unsigned int timeout;

			if (parse_positive_uint(optarg, &timeout) != 0 || timeout > INT32_MAX)
				return -1;
			options->timeout_ms = (int)timeout;
			break;
		}
		case 'p':
			if (parse_uint(optarg, &options->min_person_observations) != 0)
				return -1;
			break;
		case 'r':
			if (parse_uint(optarg, &options->min_tracking_observations) != 0)
				return -1;
			break;
		case 'H':
			usage(argv[0]);
			return 1;
		case '?':
		default:
			return -1;
		}
	}

	if (optind != argc || (have_device == have_input) ||
	    (have_device && (!options->device || !options->device[0])) ||
	    (have_input && (!options->input_path || !options->input_path[0])) ||
	    !options->model_path || !options->model_path[0] || !have_width ||
	    !have_height || !have_frames || !have_fps || options->width % 2 != 0 ||
	    options->height % 2 != 0 || options->fps > ROCKIVA_MAX_FRAMERATE ||
	    options->frames == UINT32_MAX)
		return -1;
	if (have_input && options->allow_mainpath) {
		fprintf(stderr, "--allow-mainpath is only valid with --device\n");
		return -1;
	}
	if (have_device && rockiva_probe_is_mainpath(options->device, ROCKIVA_PROBE_MAINPATH) &&
	    !options->allow_mainpath) {
		fprintf(stderr, "refusing production mainpath %s; pass --allow-mainpath "
			"only for an explicitly approved experiment\n", options->device);
		return -1;
	}
	if (options->width > UINT16_MAX || options->height > UINT16_MAX)
		return -1;
	return 0;
}

static void update_latency(uint64_t submitted, uint64_t now, uint64_t *count,
				   uint64_t *sum_ns, uint64_t *min_ns, uint64_t *max_ns,
				   const char *label)
{
	uint64_t latency;

	if (submitted == 0 || now < submitted)
		return;
	latency = now - submitted;
	(*count)++;
	*sum_ns += latency;
	if (*min_ns == 0 || latency < *min_ns)
		*min_ns = latency;
	if (latency > *max_ns)
		*max_ns = latency;
	printf("  %s_latency_ms=%.3f\n", label, (double)latency / 1000000.0);
}

static void count_person_state(struct probe_state *state,
				       const RockIvaObjectInfo *object)
{
	if (object->type != ROCKIVA_OBJECT_TYPE_PERSON)
		return;
	state->person_observations++;
	switch (object->state) {
	case ROCKIVA_OBJECT_STATE_FIRST:
		state->person_first_observations++;
		break;
	case ROCKIVA_OBJECT_STATE_TRACKING:
		state->person_tracking_observations++;
		break;
	case ROCKIVA_OBJECT_STATE_LOST:
		state->person_lost_observations++;
		break;
	case ROCKIVA_OBJECT_STATE_DISPEAR:
		state->person_disappear_observations++;
		break;
	default:
		break;
	}
}

static void print_object(const RockIvaObjectInfo *object)
{
	printf("  object obj_id=%" PRIu32 " state=%d type=%d score=%" PRIu32
	       " object_frame_id=%" PRIu32 " rect=%d,%d-%d,%d timestamp=%lu\n",
	       object->objId, object->state, object->type, object->score,
	       object->frameId, object->rect.topLeft.x, object->rect.topLeft.y,
	       object->rect.bottomRight.x, object->rect.bottomRight.y,
	       object->timestamp);
}

static struct probe_frame *find_frame_by_id_locked(struct probe_state *state,
							    uint32_t frame_id)
{
	if (frame_id == 0 || frame_id >= state->frame_count)
		return NULL;
	return &state->frames[frame_id];
}

static void detect_callback(const RockIvaDetectResult *result,
				    const RockIvaExecuteStatus status, void *userdata)
{
	struct probe_state *state = userdata;
	struct probe_frame *record = NULL;
	uint64_t now = monotonic_ns();
	uint64_t submitted;
	uint32_t i;

	if (!state) {
		fprintf(stderr, "detect callback received NULL userdata\n");
		return;
	}
	pthread_mutex_lock(&state->metrics_lock);
	state->detections++;
	if (status != ROCKIVA_SUCCESS || !result)
		state->detection_errors++;
	printf("detect status=%d frame_id=%" PRIu32 " channel_id=%" PRIu32
	       " objects=%" PRIu32 "\n",
	       status, result ? result->frameId : 0, result ? result->channelId : 0,
	       result ? result->objNum : 0);
	if (!result || status != ROCKIVA_SUCCESS) {
		signal_callbacks_locked(state);
		pthread_mutex_unlock(&state->metrics_lock);
		return;
	}
	if (result->channelId != state->channel_id)
		state->channel_mismatches++;
	/* The callback result frame ID is the only authoritative input association.
	 * RockIvaObjectInfo.frameId is intentionally not used for this lookup. */
	record = find_frame_by_id_locked(state, result->frameId);
	if (!record || record->state == PROBE_FRAME_UNUSED ||
	    record->state == PROBE_FRAME_REJECTED) {
		state->detection_unmatched++;
	} else if (record->detection_completed) {
		state->detection_duplicates++;
	} else {
		record->detection_completed = 1;
		state->detection_completed_frames++;
	}
	submitted = record ? record->submitted_ns : 0;
	update_latency(submitted, now, &state->detection_latency_count,
		       &state->detection_latency_sum_ns,
		       &state->detection_latency_min_ns,
		       &state->detection_latency_max_ns, "detect");
	if (result->objNum > ROCKIVA_MAX_OBJ_NUM)
		state->detection_object_overflows++;
	for (i = 0; i < result->objNum && i < ROCKIVA_MAX_OBJ_NUM; i++) {
		count_person_state(state, &result->objInfo[i]);
		print_object(&result->objInfo[i]);
	}
	signal_callbacks_locked(state);
	pthread_mutex_unlock(&state->metrics_lock);
}

static void release_callback(const RockIvaReleaseFrames *frames, void *userdata)
{
	struct probe_state *state = userdata;
	GstBuffer *to_unref[ROCKIVA_MAX_OBJ_NUM];
	uint32_t count;
	uint32_t i;
	uint32_t unref_count = 0;

	if (!state) {
		fprintf(stderr, "release callback received NULL userdata\n");
		return;
	}
	memset(to_unref, 0, sizeof(to_unref));
	pthread_mutex_lock(&state->metrics_lock);
	state->release_callbacks++;
	if (!frames) {
		state->release_invalid_callbacks++;
		signal_callbacks_locked(state);
		pthread_mutex_unlock(&state->metrics_lock);
		return;
	}
	state->release_entries += frames->count;
	printf("release channel_id=%" PRIu32 " count=%" PRIu32 "\n",
	       frames->channelId, frames->count);
	if (frames->channelId != state->channel_id)
		state->channel_mismatches++;
	if (frames->count == 0) {
		state->release_invalid_callbacks++;
		count = 0;
	} else {
		count = frames->count;
	}
	if (count > ROCKIVA_MAX_OBJ_NUM) {
		state->release_invalid_callbacks++;
		state->release_unmatched += count - ROCKIVA_MAX_OBJ_NUM;
		count = ROCKIVA_MAX_OBJ_NUM;
	}
	for (i = 0; i < count; i++) {
		const RockIvaImage *released = &frames->frames[i];
		struct probe_frame *record =
			find_frame_by_id_locked(state, released->frameId);
		uint64_t now = monotonic_ns();
		uint64_t submitted = record ? record->submitted_ns : 0;

		printf("  released frame_id=%" PRIu32 " fd=%" PRId32
		       " data=%p phy=%p\n", released->frameId, released->dataFd,
		       (void *)released->dataAddr, (void *)released->dataPhyAddr);
		if (!record || record->state == PROBE_FRAME_UNUSED ||
		    record->state == PROBE_FRAME_REJECTED) {
			state->release_unmatched++;
			continue;
		}
		record->release_seen = 1;
		if (record->state == PROBE_FRAME_RELEASED) {
			state->release_duplicates++;
			continue;
		}
		if (released->channelId != state->channel_id ||
		    released->dataFd != record->fd || released->dataAddr != NULL ||
		    released->dataPhyAddr != NULL || record->buffer == NULL) {
			state->release_mismatches++;
			state->release_unmatched++;
			fprintf(stderr, "release mismatch frame_id=%" PRIu32
				" expected_fd=%" PRId32 " actual_fd=%" PRId32 "\n",
				released->frameId, record->fd, released->dataFd);
			continue;
		}
		update_latency(submitted, now, &state->release_latency_count,
			       &state->release_latency_sum_ns,
			       &state->release_latency_min_ns,
			       &state->release_latency_max_ns, "release");
		to_unref[unref_count++] = record->buffer;
		record->buffer = NULL;
		record->state = PROBE_FRAME_RELEASED;
		state->released_frames++;
	}
	pthread_mutex_unlock(&state->metrics_lock);

	/* Do not run GStreamer finalizers while holding the callback accounting
	 * mutex.  Signal only after the unrefs, so cleanup cannot race this callback
	 * returning to RockIVA. */
	for (i = 0; i < unref_count; i++)
		gst_buffer_unref(to_unref[i]);
	pthread_mutex_lock(&state->metrics_lock);
	signal_callbacks_locked(state);
	pthread_mutex_unlock(&state->metrics_lock);
}

static gboolean propose_allocation(GstAppSink *appsink, GstQuery *query,
					   gpointer user_data)
{
	GstCaps *caps = NULL;
	GstVideoInfo info;
	GstBufferPool *pool;
	GstAllocator *allocator;
	GstStructure *config;
	GstAllocationParams params = { 0 };
	gsize size;

	(void)appsink;
	(void)user_data;
	if (!query)
		return FALSE;
	gst_query_parse_allocation(query, &caps, NULL);
	if (!caps || !gst_video_info_from_caps(&info, caps))
		return FALSE;

	/* The driver exposes NV12M as two physical DMA-BUFs, while RockIvaImage has
	 * one dataFd.  Advertise one downstream DMA-BUF pool and omit VIDEO_META
	 * from this query so v4l2src copies its multi-plane capture into that pool
	 * instead of sharing its incompatible producer pool. */
	pool = gst_video_buffer_pool_new();
	if (!pool)
		return FALSE;
	allocator = probe_dmabuf_allocator_new();
	if (!allocator) {
		gst_object_unref(pool);
		return FALSE;
	}
	size = GST_VIDEO_INFO_SIZE(&info);
	config = gst_buffer_pool_get_config(pool);
	gst_buffer_pool_config_set_allocator(config, allocator, &params);
	gst_buffer_pool_config_set_params(config, caps, size, 0, 0);
	if (!gst_buffer_pool_set_config(pool, config)) {
		gst_object_unref(allocator);
		gst_object_unref(pool);
		return FALSE;
	}
	gst_query_add_allocation_pool(query, pool, size, 0, 0);
	gst_query_add_allocation_param(query, allocator, &params);
	printf("appsink allocation=single-memory-dmabuf-copy heap=%s size=%zu\n",
	       PROBE_DMABUF_ALLOCATOR(allocator)->heap_path, size);
	gst_object_unref(allocator);
	gst_object_unref(pool);
	return TRUE;
}

static void describe_sample_memories(GstBuffer *buffer)
{
	GstVideoMeta *meta;
	guint i;

	if (!buffer)
		return;
	printf("sample_memory_layout count=%u buffer_size=%zu\n",
	       gst_buffer_n_memory(buffer), gst_buffer_get_size(buffer));
	for (i = 0; i < gst_buffer_n_memory(buffer); i++) {
		GstMemory *memory = gst_buffer_peek_memory(buffer, i);
		gsize offset = 0;
		gsize maxsize = 0;
		gsize size = memory ? gst_memory_get_sizes(memory, &offset, &maxsize) : 0;
		int fd = -1;

		if (memory && gst_is_dmabuf_memory(memory))
			fd = gst_dmabuf_memory_get_fd(memory);
		printf("  memory index=%u dmabuf=%u fd=%d offset=%zu size=%zu "
		       "maxsize=%zu allocator=%s\n", i,
		       memory && gst_is_dmabuf_memory(memory), fd, offset, size, maxsize,
		       memory && memory->allocator ? GST_OBJECT_NAME(memory->allocator) :
		       "(none)");
	}
	meta = gst_buffer_get_video_meta(buffer);
	if (meta) {
		printf("sample_video_meta format=%u planes=%u width=%u height=%u "
		       "offset0=%zu offset1=%zu stride0=%d stride1=%d\n",
		       meta->format, meta->n_planes, meta->width, meta->height,
		       meta->offset[0], meta->offset[1], meta->stride[0], meta->stride[1]);
	}
}

static void on_mp4_demux_pad_added(GstElement *demux, GstPad *new_pad,
					   gpointer user_data)
{
	GstElement *parser = GST_ELEMENT(user_data);
	GstCaps *caps = NULL;
	const GstStructure *structure;
	const gchar *name = NULL;
	GstPad *sink_pad;
	GstPadLinkReturn link_result;

	(void)demux;
	caps = gst_pad_get_current_caps(new_pad);
	if (!caps)
		caps = gst_pad_query_caps(new_pad, NULL);
	if (caps && gst_caps_get_size(caps) > 0) {
		structure = gst_caps_get_structure(caps, 0);
		name = gst_structure_get_name(structure);
	}
	if ((!name || !g_str_has_prefix(name, "video/")) &&
	    !g_str_has_prefix(GST_PAD_NAME(new_pad), "video")) {
		if (caps)
			gst_caps_unref(caps);
		return;
	}
	sink_pad = gst_element_get_static_pad(parser, "sink");
	if (!sink_pad) {
		if (caps)
			gst_caps_unref(caps);
		fprintf(stderr, "cannot obtain h264parse sink pad\n");
		return;
	}
	if (!gst_pad_is_linked(sink_pad)) {
		link_result = gst_pad_link(new_pad, sink_pad);
		if (link_result != GST_PAD_LINK_OK)
			fprintf(stderr, "failed to link MP4 video pad: %s\n",
				gst_pad_link_get_name(link_result));
	}
	gst_object_unref(sink_pad);
	if (caps)
		gst_caps_unref(caps);
}

static GstElement *create_pipeline(const struct probe_options *options)
{
	GstElement *pipeline = NULL;
	GstElement *source = NULL;
	GstElement *demux = NULL;
	GstElement *parser = NULL;
	GstElement *decoder = NULL;
	GstElement *capsfilter = NULL;
	GstElement *appsink = NULL;
	GstCaps *caps = NULL;
	GstAppSinkCallbacks callbacks = { 0 };
	int elements_added = 0;

	if (options->input_kind == PROBE_INPUT_MP4) {
		pipeline = gst_pipeline_new("rockiva-mp4-dmabuf-probe");
		source = gst_element_factory_make("filesrc", "mp4-source");
		demux = gst_element_factory_make("qtdemux", "mp4-demux");
		parser = gst_element_factory_make("h264parse", "mp4-parser");
		decoder = gst_element_factory_make("mppvideodec", "mp4-decoder");
	} else {
		pipeline = gst_pipeline_new("rockiva-dmabuf-probe");
		source = gst_element_factory_make("v4l2src", "probe-source");
	}
	capsfilter = gst_element_factory_make("capsfilter", "probe-caps");
	appsink = gst_element_factory_make("appsink", "probe-sink");
	if (!pipeline || !source || !capsfilter || !appsink ||
	    (options->input_kind == PROBE_INPUT_MP4 &&
	     (!demux || !parser || !decoder))) {
		fprintf(stderr, "required GStreamer elements are unavailable\n");
		goto create_failed;
	}

	if (options->input_kind == PROBE_INPUT_MP4) {
		/* MP4 mode is a decoder-output experiment, not a V4L2 import path. */
		g_object_set(source, "location", options->input_path, NULL);
		g_object_set(decoder, "arm-afbc", FALSE, "dma-feature", TRUE, NULL);
		caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
					   "width", G_TYPE_INT, (gint)options->width,
					   "height", G_TYPE_INT, (gint)options->height,
					   "interlace-mode", G_TYPE_STRING, "progressive", NULL);
	} else {
		/* This is the capture/export mode, not dmabuf-import. */
		g_object_set(source, "device", options->device,
			     "io-mode", GST_V4L2_IO_MODE_DMABUF, NULL);
		caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "NV12",
					   "width", G_TYPE_INT, (gint)options->width,
					   "height", G_TYPE_INT, (gint)options->height,
					   "framerate", GST_TYPE_FRACTION, (gint)options->fps, 1,
					   NULL);
	}
	if (!caps) {
		fprintf(stderr, "cannot allocate DMA-BUF caps\n");
		goto create_failed;
	}
	g_object_set(capsfilter, "caps", caps, NULL);
	gst_caps_unref(caps);
	g_object_set(appsink, "emit-signals", FALSE, "sync", FALSE,
		     "max-buffers", 4U, "drop", FALSE, "wait-on-eos", FALSE,
		     "enable-last-sample", FALSE, NULL);
	callbacks.propose_allocation = propose_allocation;
	gst_app_sink_set_callbacks(GST_APP_SINK(appsink), &callbacks, NULL, NULL);

	if (options->input_kind == PROBE_INPUT_MP4) {
		gst_bin_add_many(GST_BIN(pipeline), source, demux, parser, decoder,
				 capsfilter, appsink, NULL);
	} else {
		gst_bin_add_many(GST_BIN(pipeline), source, capsfilter, appsink, NULL);
	}
	elements_added = 1;
	if (options->input_kind == PROBE_INPUT_MP4) {
		if (!gst_element_link(source, demux) ||
		    !gst_element_link(parser, decoder) ||
		    !gst_element_link_many(decoder, capsfilter, appsink, NULL)) {
			fprintf(stderr, "failed to link MP4 DMA-BUF pipeline\n");
			goto pipeline_failed;
		}
		g_signal_connect(demux, "pad-added",
				 G_CALLBACK(on_mp4_demux_pad_added), parser);
		printf("pipeline=filesrc location=%s ! qtdemux ! h264parse ! "
		       "mppvideodec(dma-feature=true) ! "
		       "video/x-raw,format=NV12,width=%u,height=%u "
		       "appsink\n", options->input_path, options->width,
		       options->height);
	} else {
		if (!gst_element_link_many(source, capsfilter, appsink, NULL)) {
			fprintf(stderr, "failed to link v4l2src -> capsfilter -> appsink\n");
			goto pipeline_failed;
		}
		printf("pipeline=v4l2src device=%s io-mode=dmabuf ! "
		       "video/x-raw,format=NV12,width=%u,height=%u,"
		       "framerate=%u/1 ! appsink\n",
		       options->device, options->width, options->height, options->fps);
	}
	return pipeline;

pipeline_failed:
	if (elements_added)
		gst_object_unref(pipeline);
	return NULL;

create_failed:
	if (caps)
		gst_caps_unref(caps);
	if (pipeline)
		gst_object_unref(pipeline);
	if (appsink)
		gst_object_unref(appsink);
	if (capsfilter)
		gst_object_unref(capsfilter);
	if (decoder)
		gst_object_unref(decoder);
	if (parser)
		gst_object_unref(parser);
	if (demux)
		gst_object_unref(demux);
	if (source)
		gst_object_unref(source);
	return NULL;
}

static void report_bus_message(GstMessage *message)
{
	GError *error = NULL;
	gchar *debug = NULL;

	if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
		gst_message_parse_error(message, &error, &debug);
		fprintf(stderr, "gstreamer error from %s: %s%s%s%s\n",
			GST_OBJECT_NAME(message->src), error ? error->message : "unknown",
			debug ? " (" : "", debug ? debug : "", debug ? ")" : "");
		if (error)
			g_error_free(error);
		g_free(debug);
	} else if (GST_MESSAGE_EOS == GST_MESSAGE_TYPE(message)) {
		fprintf(stderr, "gstreamer reached EOS before all requested samples\n");
	}
}

static int drain_bus(GstBus *bus)
{
	GstMessage *message;
	int failed = 0;

	if (!bus)
		return 0;
	while ((message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS))) {
		failed = 1;
		report_bus_message(message);
		gst_message_unref(message);
	}
	return failed;
}

static int start_pipeline(GstElement *pipeline, int timeout_ms)
{
	GstStateChangeReturn result;
	GstState state = GST_STATE_NULL;
	GstState pending = GST_STATE_VOID_PENDING;
	GstClockTime timeout = (GstClockTime)timeout_ms * GST_MSECOND;

	result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
	if (result == GST_STATE_CHANGE_FAILURE) {
		fprintf(stderr, "failed to set capture pipeline PLAYING\n");
		return -1;
	}
	result = gst_element_get_state(pipeline, &state, &pending, timeout);
	if (result == GST_STATE_CHANGE_FAILURE || result == GST_STATE_CHANGE_ASYNC ||
	    state != GST_STATE_PLAYING) {
		fprintf(stderr, "capture pipeline did not reach PLAYING: result=%d state=%d "
			"pending=%d\n", result, state, pending);
		return -1;
	}
	printf("pipeline_state=PLAYING\n");
	return 0;
}

static int stop_pipeline(GstElement *pipeline, int timeout_ms)
{
	GstStateChangeReturn result;
	GstState state = GST_STATE_VOID_PENDING;
	GstState pending = GST_STATE_VOID_PENDING;
	GstClockTime timeout = (GstClockTime)timeout_ms * GST_MSECOND;

	result = gst_element_set_state(pipeline, GST_STATE_NULL);
	if (result == GST_STATE_CHANGE_FAILURE) {
		fprintf(stderr, "failed to set capture pipeline NULL\n");
		return -1;
	}
	result = gst_element_get_state(pipeline, &state, &pending, timeout);
	if (result == GST_STATE_CHANGE_FAILURE || result == GST_STATE_CHANGE_ASYNC ||
	    state != GST_STATE_NULL) {
		fprintf(stderr, "capture pipeline did not reach NULL: result=%d state=%d "
			"pending=%d\n", result, state, pending);
		return -1;
	}
	printf("pipeline_state=NULL\n");
	return 0;
}

static int inspect_sample(GstSample *sample, const struct probe_options *options,
				  struct frame_layout *layout, char *reason, size_t reason_size)
{
	GstBuffer *buffer;
	GstCaps *caps;
	GstVideoInfo video_info;
	GstVideoMeta *meta;
	GstMemory *memory;
	gsize memory_offset = 0;
	gsize memory_maxsize = 0;
	gsize visible_size;
	uint64_t hstride;
	uint64_t uv_size;
	uint64_t required_size;

	if (!sample || !layout)
		goto invalid;
	buffer = gst_sample_get_buffer(sample);
	caps = gst_sample_get_caps(sample);
	if (!buffer || !caps || gst_caps_get_size(caps) != 1 ||
	    !gst_video_info_from_caps(&video_info, caps))
		goto invalid;
	if (GST_VIDEO_INFO_FORMAT(&video_info) != GST_VIDEO_FORMAT_NV12)
		goto format_invalid;
	if ((guint)GST_VIDEO_INFO_WIDTH(&video_info) != options->width ||
	    (guint)GST_VIDEO_INFO_HEIGHT(&video_info) != options->height)
		goto dimensions_invalid;
	if (GST_VIDEO_INFO_INTERLACE_MODE(&video_info) !=
	    GST_VIDEO_INTERLACE_MODE_PROGRESSIVE)
		goto interlace_invalid;
	if (gst_buffer_n_memory(buffer) != 1) {
		describe_sample_memories(buffer);
		goto memory_count_invalid;
	}
	memory = gst_buffer_peek_memory(buffer, 0);
	if (!memory || !gst_is_dmabuf_memory(memory)) {
		describe_sample_memories(buffer);
		goto dmabuf_invalid;
	}
	memset(layout, 0, sizeof(*layout));
	layout->width = GST_VIDEO_INFO_WIDTH(&video_info);
	layout->height = GST_VIDEO_INFO_HEIGHT(&video_info);
	layout->stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 0);
	layout->uv_stride = GST_VIDEO_INFO_PLANE_STRIDE(&video_info, 1);
	layout->offset = GST_VIDEO_INFO_PLANE_OFFSET(&video_info, 0);
	layout->uv_offset = GST_VIDEO_INFO_PLANE_OFFSET(&video_info, 1);
	layout->logical_planes = GST_VIDEO_INFO_N_PLANES(&video_info);
	layout->has_video_meta = 0;
	if (layout->logical_planes != 2)
		goto logical_planes_invalid;
	meta = gst_buffer_get_video_meta(buffer);
	if (meta) {
		/* NV12 has two logical planes, but both must live in the one physical
		 * DMA-BUF memory accepted by RockIvaImage.dataFd. */
		if (meta->format != GST_VIDEO_FORMAT_NV12 || meta->n_planes != 2)
			goto meta_logical_planes_invalid;
		if (meta->width != options->width || meta->height != options->height)
			goto dimensions_invalid;
		layout->width = meta->width;
		layout->height = meta->height;
		layout->stride = meta->stride[0];
		layout->uv_stride = meta->stride[1];
		layout->offset = meta->offset[0];
		layout->uv_offset = meta->offset[1];
		layout->logical_planes = meta->n_planes;
		layout->has_video_meta = 1;
	}
	if (layout->stride <= 0 || layout->uv_stride <= 0 ||
	    layout->stride < (gint)layout->width ||
	    layout->uv_stride < (gint)layout->width ||
	    layout->stride > UINT16_MAX || layout->uv_stride > UINT16_MAX)
		goto stride_invalid;
	if (layout->uv_stride != layout->stride)
		goto uv_stride_invalid;
	if (layout->offset != 0)
		goto offset_invalid;
	if (layout->uv_offset < layout->offset ||
	    (layout->uv_offset - layout->offset) % (gsize)layout->stride != 0)
		goto uv_offset_invalid;
	hstride = (uint64_t)((layout->uv_offset - layout->offset) /
				    (gsize)layout->stride);
	if (hstride < layout->height || hstride > UINT16_MAX)
		goto hstride_invalid;
	if (hstride > UINT64_MAX / (uint64_t)(guint)layout->stride)
		goto size_invalid;
	if (hstride * (uint64_t)(guint)layout->stride != layout->uv_offset)
		goto uv_offset_invalid;
	uv_size = (uint64_t)(guint)layout->uv_stride * (layout->height / 2U);
	if ((uint64_t)layout->uv_offset > UINT64_MAX - uv_size)
		goto size_invalid;
	required_size = (uint64_t)layout->uv_offset + uv_size;
	if (required_size > G_MAXSIZE)
		goto size_invalid;
	layout->hstride = (guint)hstride;
	visible_size = gst_memory_get_sizes(memory, &memory_offset, &memory_maxsize);
	if (memory_offset != 0 || visible_size < (gsize)required_size ||
	    gst_buffer_get_size(buffer) < (gsize)required_size)
		goto size_invalid;
	layout->visible_size = visible_size;
	layout->max_size = memory_maxsize;
	layout->fd = gst_dmabuf_memory_get_fd(memory);
	if (layout->fd < 0)
		goto fd_invalid;
	return 0;

format_invalid:
	snprintf(reason, reason_size, "caps are not NV12");
	return -1;
dimensions_invalid:
	snprintf(reason, reason_size, "negotiated dimensions differ from request");
	return -1;
interlace_invalid:
	snprintf(reason, reason_size, "interlaced input is not accepted");
	return -1;
memory_count_invalid:
	snprintf(reason, reason_size, "buffer has %u memories, expected one",
		 gst_buffer_n_memory(buffer));
	return -1;
dmabuf_invalid:
	snprintf(reason, reason_size, "buffer memory is not DMA-BUF");
	return -1;
meta_logical_planes_invalid:
	snprintf(reason, reason_size,
		 "GstVideoMeta must describe two logical NV12 planes");
	return -1;
logical_planes_invalid:
	snprintf(reason, reason_size, "NV12 must describe two logical planes");
	return -1;
stride_invalid:
	snprintf(reason, reason_size,
		 "NV12 plane strides must be positive, <= uint16_t, and at least width");
	return -1;
uv_stride_invalid:
	snprintf(reason, reason_size, "NV12 Y and UV strides must match");
	return -1;
offset_invalid:
	snprintf(reason, reason_size, "NV12 Y plane offset is nonzero");
	return -1;
uv_offset_invalid:
	snprintf(reason, reason_size,
		 "NV12 UV plane must follow Y by stride*hstride");
	return -1;
hstride_invalid:
	snprintf(reason, reason_size,
		 "NV12 hstride is smaller than height or exceeds uint16_t");
	return -1;
size_invalid:
	snprintf(reason, reason_size,
		 "DMA-BUF size is smaller than the complete NV12 plane layout");
	return -1;
fd_invalid:
	snprintf(reason, reason_size, "DMA-BUF fd is invalid");
	return -1;
invalid:
	snprintf(reason, reason_size, "sample has no buffer/caps");
	return -1;
}

static GstBuffer *mark_push_failure_locked(struct probe_state *state,
						   uint32_t frame_id)
{
	struct probe_frame *record;
	GstBuffer *buffer = NULL;

	if (frame_id == 0 || frame_id >= state->frame_count)
		return NULL;
	record = &state->frames[frame_id];
	if (record->state == PROBE_FRAME_PENDING) {
		/* A callback before a nonzero return means the SDK may have accepted
		 * the buffer despite reporting failure.  Keep the reference until a
		 * matching release callback rather than risking a DMA-BUF use-after-free. */
		if (record->detection_completed || record->release_seen) {
			record->state = PROBE_FRAME_ACCEPTED;
			return NULL;
		}
		buffer = record->buffer;
		record->buffer = NULL;
		record->state = PROBE_FRAME_REJECTED;
	}
	return buffer;
}

static enum wait_finish_state wait_finish(RockIvaHandle handle, int timeout_ms,
						  const char *label)
{
	RockIvaRetCode ret = ROCKIVA_WaitFinish(handle, -1, timeout_ms);

	if (ret == ROCKIVA_RET_SUCCESS) {
		printf("%s ret=%d status=success\n", label, ret);
		return WAIT_FINISH_SUCCESS;
	}
	if (ret == ROCKIVA_RET_UNSUPPORTED) {
		printf("%s ret=%d status=unsupported capability fallback\n", label, ret);
		return WAIT_FINISH_UNSUPPORTED;
	}
	printf("%s ret=%d status=failure\n", label, ret);
	return WAIT_FINISH_FAILED;
}

int main(int argc, char **argv)
{
	struct probe_options options;
	static struct probe_state state;
	struct frame_layout first_layout;
	struct frame_layout layout;
	RockIvaInitParam init_params;
	RockIvaDetTaskParams det_params;
	RockIvaHandle handle = NULL;
	RockIvaRetCode ret;
	GstElement *pipeline = NULL;
	GstAppSink *appsink = NULL;
	GstBus *bus = NULL;
	GstSample *first_sample = NULL;
	GstSample *sample = NULL;
	pthread_condattr_t cond_attr;
	uint32_t frame;
	char version[MAX_VERSION] = {0};
	char reason[160];
	int parse_result;
	int cond_attr_result;
	int cond_attr_initialized = 0;
	int operation_failed = 0;
	int metrics_lock_initialized = 0;
	int callbacks_cond_initialized = 0;
	int handle_initialized = 0;
	int detect_initialized = 0;
	int release_callback_initialized = 0;
	int initial_cleanup_safe = 0;
	int final_cleanup_safe = 0;
	int release_succeeded = 0;
	int pipeline_started = 0;
	int pipeline_cleanup_safe = 0;
	enum wait_finish_state wait_state = WAIT_FINISH_NOT_ATTEMPTED;
	enum wait_finish_state final_wait_state = WAIT_FINISH_NOT_ATTEMPTED;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0)
		return parse_result > 0 ? 0 : 2;

	memset(&state, 0, sizeof(state));
	if (pthread_mutex_init(&state.metrics_lock, NULL) != 0) {
		fprintf(stderr, "cannot initialize metrics lock\n");
		return 2;
	}
	metrics_lock_initialized = 1;
	cond_attr_result = pthread_condattr_init(&cond_attr);
	if (cond_attr_result == 0) {
		cond_attr_initialized = 1;
		cond_attr_result = pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
	}
	if (cond_attr_result == 0)
		cond_attr_result = pthread_cond_init(&state.callbacks_cond, &cond_attr);
	if (cond_attr_result == 0)
		state.callbacks_cond_initialized = 1;
	if (cond_attr_initialized)
		(void)pthread_condattr_destroy(&cond_attr);
	if (cond_attr_result != 0) {
		fprintf(stderr, "cannot initialize callback condition variable: %s\n",
			strerror(cond_attr_result));
		operation_failed = 1;
		goto done;
	}
	callbacks_cond_initialized = 1;
	state.channel_id = options.channel_id;
	state.frame_count = (size_t)options.frames + 1U;
	if (state.frame_count <= (size_t)options.frames ||
	    state.frame_count > SIZE_MAX / sizeof(*state.frames)) {
		fprintf(stderr, "frame count is too large\n");
		operation_failed = 1;
		goto done;
	}
	state.frames = calloc(state.frame_count, sizeof(*state.frames));
	if (!state.frames) {
		fprintf(stderr, "cannot allocate frame ownership table\n");
		operation_failed = 1;
		goto done;
	}

	gst_init(NULL, NULL);
	pipeline = create_pipeline(&options);
	if (!pipeline) {
		operation_failed = 1;
		goto done;
	}
	appsink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(pipeline), "probe-sink"));
	bus = gst_element_get_bus(pipeline);
	if (!appsink || !bus) {
		fprintf(stderr, "cannot obtain appsink or pipeline bus\n");
		operation_failed = 1;
		goto done;
	}
	pipeline_started = 1;
	if (start_pipeline(pipeline, options.timeout_ms) != 0) {
		operation_failed = 1;
		goto done;
	}
	first_sample = gst_app_sink_try_pull_sample(appsink,
						    (GstClockTime)options.timeout_ms * GST_MSECOND);
	if (!first_sample) {
		if (drain_bus(bus) == 0)
			fprintf(stderr, "timed out waiting for first DMA-BUF sample\n");
		operation_failed = 1;
		goto done;
	}
	state.samples_received++;
	if (inspect_sample(first_sample, &options, &first_layout, reason,
				   sizeof(reason)) != 0) {
		fprintf(stderr, "reject sample 1: %s\n", reason);
		state.samples_rejected++;
		operation_failed = 1;
		goto done;
	}
	printf("negotiated width=%u height=%u hstride=%u y_stride=%d uv_stride=%d "
	       "y_offset=%zu uv_offset=%zu logical_planes=%u video_meta=%u "
	       "sample_size=%zu max_size=%zu fd=%" PRId32 "\n",
	       first_layout.width, first_layout.height, first_layout.hstride,
	       first_layout.stride, first_layout.uv_stride, first_layout.offset,
	       first_layout.uv_offset, first_layout.logical_planes,
	       first_layout.has_video_meta, first_layout.visible_size,
	       first_layout.max_size, first_layout.fd);

	memset(&init_params, 0, sizeof(init_params));
	init_params.logLevel = ROCKIVA_LOG_ERROR;
	ret = snprintf(init_params.modelPath, sizeof(init_params.modelPath), "%s",
		       options.model_path);
	if (ret < 0 || (size_t)ret >= sizeof(init_params.modelPath)) {
		fprintf(stderr, "model path is too long for RockIVA\n");
		operation_failed = 1;
		goto done;
	}
	init_params.coreMask = options.core_mask;
	init_params.channelId = options.channel_id;
	init_params.detModel = options.model;
	init_params.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
	init_params.imageInfo.width = (uint16_t)first_layout.width;
	init_params.imageInfo.height = (uint16_t)first_layout.height;
	if (first_layout.stride < 0 || (uint64_t)first_layout.stride > UINT16_MAX) {
		fprintf(stderr, "DMA-BUF wstride=%d cannot be represented as uint16_t\n",
			first_layout.stride);
		operation_failed = 1;
		goto done;
	}
	init_params.imageInfo.wstride = (uint16_t)first_layout.stride;
	init_params.imageInfo.hstride = (uint16_t)first_layout.hstride;
	init_params.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
	init_params.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
	memset(&det_params, 0, sizeof(det_params));
	det_params.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);

	if (options.input_kind == PROBE_INPUT_MP4) {
		printf("probe mode=mp4-dmabuf model=%s input=%s model_path=%s "
		       "width=%u height=%u frames=%u fps=%u channel=%u core_mask=0x%x "
		       "min_person=%u min_tracking=%u\n",
		       model_name(options.model), options.input_path, options.model_path,
		       options.width, options.height, options.frames, options.fps,
		       options.channel_id, options.core_mask,
		       options.min_person_observations, options.min_tracking_observations);
	} else {
		printf("probe mode=dmabuf model=%s device=%s model_path=%s width=%u height=%u "
		       "frames=%u fps=%u channel=%u core_mask=0x%x min_person=%u "
		       "min_tracking=%u\n",
		       model_name(options.model), options.device, options.model_path,
		       options.width, options.height, options.frames, options.fps,
		       options.channel_id, options.core_mask,
		       options.min_person_observations, options.min_tracking_observations);
	}
	ret = ROCKIVA_GetVersion(MAX_VERSION, version);
	printf("rockiva version ret=%d value=%s\n", ret,
	       version[0] ? version : "(unavailable)");
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_GetVersion failed: %d\n", ret);
		operation_failed = 1;
		goto done;
	}
	ret = ROCKIVA_Init(&handle, ROCKIVA_MODE_VIDEO, &init_params, &state);
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_Init failed: %d\n", ret);
		operation_failed = 1;
		goto done;
	}
	handle_initialized = 1;
	ret = ROCKIVA_DETECT_Init(handle, &det_params, detect_callback);
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_DETECT_Init failed: %d\n", ret);
		operation_failed = 1;
		goto sdk_cleanup;
	}
	detect_initialized = 1;
	ret = ROCKIVA_SetFrameReleaseCallback(handle, release_callback);
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_SetFrameReleaseCallback failed: %d\n", ret);
		operation_failed = 1;
		goto sdk_cleanup;
	}
	release_callback_initialized = 1;

	for (frame = 1; frame <= options.frames; frame++) {
		RockIvaImage image;
		GstBuffer *held_buffer;
		GstBuffer *failed_buffer;
		struct probe_frame *record;
		uint64_t submitted;

		if (frame == 1) {
			sample = first_sample;
			first_sample = NULL;
		} else {
			sample = gst_app_sink_try_pull_sample(appsink,
							    (GstClockTime)options.timeout_ms * GST_MSECOND);
			if (!sample) {
				if (drain_bus(bus) == 0)
					fprintf(stderr, "timed out waiting for sample %u\n", frame);
				operation_failed = 1;
				break;
			}
			state.samples_received++;
		}
		if (inspect_sample(sample, &options, &layout, reason, sizeof(reason)) != 0) {
			fprintf(stderr, "reject sample %u: %s\n", frame, reason);
			state.samples_rejected++;
			operation_failed = 1;
			gst_sample_unref(sample);
			sample = NULL;
			break;
		}
		if (layout.width != first_layout.width || layout.height != first_layout.height ||
		    layout.hstride != first_layout.hstride ||
		    layout.stride != first_layout.stride ||
		    layout.uv_stride != first_layout.uv_stride ||
		    layout.offset != first_layout.offset ||
		    layout.uv_offset != first_layout.uv_offset ||
		    layout.logical_planes != first_layout.logical_planes ||
		    layout.has_video_meta != first_layout.has_video_meta || layout.fd < 0) {
			fprintf(stderr, "reject sample %u: negotiated DMA-BUF layout changed "
				"(width=%u height=%u hstride=%u y_stride=%d uv_stride=%d "
				"y_offset=%zu uv_offset=%zu logical_planes=%u video_meta=%u "
				"fd=%" PRId32 ")\n", frame, layout.width, layout.height,
				layout.hstride, layout.stride, layout.uv_stride, layout.offset,
				layout.uv_offset, layout.logical_planes, layout.has_video_meta,
				layout.fd);
			state.samples_rejected++;
			operation_failed = 1;
			gst_sample_unref(sample);
			sample = NULL;
			break;
		}
		held_buffer = gst_buffer_ref(gst_sample_get_buffer(sample));
		memset(&image, 0, sizeof(image));
		image.frameId = frame;
		image.channelId = options.channel_id;
		image.info = init_params.imageInfo;
		image.size = 0;
		image.dataAddr = NULL;
		image.dataPhyAddr = NULL;
		image.dataFd = layout.fd;
		submitted = monotonic_ns();
		pthread_mutex_lock(&state.metrics_lock);
		record = &state.frames[frame];
		record->buffer = held_buffer;
		record->fd = layout.fd;
		record->width = layout.width;
		record->height = layout.height;
		record->hstride = layout.hstride;
		record->stride = layout.stride;
		record->uv_stride = layout.uv_stride;
		record->offset = layout.offset;
		record->uv_offset = layout.uv_offset;
		record->visible_size = layout.visible_size;
		record->max_size = layout.max_size;
		record->logical_planes = layout.logical_planes;
		record->has_video_meta = layout.has_video_meta;
		record->pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
		record->submitted_ns = submitted;
		record->state = PROBE_FRAME_PENDING;
		record->detection_completed = 0;
		record->release_seen = 0;
		pthread_mutex_unlock(&state.metrics_lock);
		ret = ROCKIVA_PushFrame(handle, &image, NULL);
		failed_buffer = NULL;
		pthread_mutex_lock(&state.metrics_lock);
		if (ret != ROCKIVA_RET_SUCCESS) {
			state.push_failures++;
			failed_buffer = mark_push_failure_locked(&state, frame);
		} else {
			state.pushed++;
			if (record->state == PROBE_FRAME_PENDING)
				record->state = PROBE_FRAME_ACCEPTED;
		}
		pthread_mutex_unlock(&state.metrics_lock);
		if (failed_buffer)
			gst_buffer_unref(failed_buffer);
		gst_sample_unref(sample);
		sample = NULL;
		if (ret != ROCKIVA_RET_SUCCESS) {
			fprintf(stderr, "push frame %u failed: %d\n", frame, ret);
			operation_failed = 1;
			break;
		}
		printf("push frame_id=%u fd=%" PRId32 " width=%u height=%u hstride=%u "
		       "y_stride=%d uv_stride=%d y_offset=%zu uv_offset=%zu ret=%d\n",
		       frame, layout.fd, layout.width, layout.height, layout.hstride,
		       layout.stride, layout.uv_stride, layout.offset, layout.uv_offset, ret);
	}
	first_sample = NULL;
	if (operation_failed && !handle_initialized)
		goto done;

sdk_cleanup:
	if (handle_initialized) {
		wait_state = wait_finish(handle, options.timeout_ms, "wait_finish");
		if (wait_state == WAIT_FINISH_SUCCESS) {
			initial_cleanup_safe = 1;
		} else if (wait_state == WAIT_FINISH_UNSUPPORTED) {
			printf("wait_finish capability fallback: waiting for callback completion\n");
			if (wait_for_callback_completion(&state, options.timeout_ms) == 0)
				initial_cleanup_safe = 1;
			else
				operation_failed = 1;
		} else {
			operation_failed = 1;
		}
		if (detect_initialized) {
			if (!initial_cleanup_safe) {
				fprintf(stderr, "skipping ROCKIVA_DETECT_Release: callback completion "
					"not confirmed\n");
			} else {
				ret = ROCKIVA_DETECT_Release(handle);
				printf("detect_release ret=%d\n", ret);
				if (ret != ROCKIVA_RET_SUCCESS)
					operation_failed = 1;
			}
		}
		if (!initial_cleanup_safe) {
			fprintf(stderr, "skipping final_wait_finish and ROCKIVA_Release after "
				"incomplete callback completion\n");
			goto done;
		}
		final_wait_state = wait_finish(handle, options.timeout_ms, "final_wait_finish");
		if (final_wait_state == WAIT_FINISH_SUCCESS) {
			final_cleanup_safe = 1;
		} else if (final_wait_state == WAIT_FINISH_UNSUPPORTED) {
			printf("final_wait_finish capability fallback: waiting for callback completion\n");
			if (wait_for_callback_completion(&state, options.timeout_ms) == 0)
				final_cleanup_safe = 1;
			else
				operation_failed = 1;
		} else {
			operation_failed = 1;
		}
		if (final_cleanup_safe) {
			ret = ROCKIVA_Release(handle);
			printf("release ret=%d\n", ret);
			if (ret != ROCKIVA_RET_SUCCESS)
				operation_failed = 1;
			else
				release_succeeded = 1;
		} else {
			fprintf(stderr, "skipping ROCKIVA_Release after wait failure\n");
		}
	}

done:
	if (first_sample)
		gst_sample_unref(first_sample);
	if (sample)
		gst_sample_unref(sample);
	if (handle_initialized && (!final_cleanup_safe || !release_succeeded))
		fprintf(stderr, "DMA-BUF ownership cleanup deferred after incomplete SDK shutdown\n");
	if (handle_initialized && release_callback_initialized &&
	    (!final_cleanup_safe || !release_succeeded))
		fprintf(stderr, "unreturned DMA-BUF buffers remain held by RockIVA\n");
	pthread_mutex_lock(&state.metrics_lock);
	if (!callbacks_complete_locked(&state)) {
		fprintf(stderr, "callback accounting invariant failed: pushed=%" PRIu64
			       " detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
		       state.pushed, state.detection_completed_frames, state.released_frames);
		operation_failed = 1;
	}
	if (state.pushed != options.frames || state.push_failures != 0 ||
	    state.samples_rejected != 0 || state.released_frames != state.pushed ||
	    state.detection_errors != 0 || state.detection_frame_errors != 0 ||
	    state.detection_object_overflows != 0 || state.release_unmatched != 0 ||
	    state.release_duplicates != 0 || state.release_mismatches != 0 ||
	    state.release_invalid_callbacks != 0 || state.channel_mismatches != 0)
		operation_failed = 1;
	if (state.person_observations < options.min_person_observations) {
		fprintf(stderr, "person observations below minimum: %" PRIu64 " < %u\n",
			state.person_observations, options.min_person_observations);
		operation_failed = 1;
	}
	if (state.person_tracking_observations < options.min_tracking_observations) {
		fprintf(stderr, "person tracking observations below minimum: %" PRIu64
			       " < %u\n", state.person_tracking_observations,
		       options.min_tracking_observations);
		operation_failed = 1;
	}
	if (handle_initialized && (!final_cleanup_safe || !release_succeeded))
		operation_failed = 1;
	printf("summary mode=%s samples_received=%" PRIu64 " samples_rejected=%" PRIu64
	       " pushed=%" PRIu64 " push_failures=%" PRIu64
	       " detection_callbacks=%" PRIu64 " detection_errors=%" PRIu64
	       " person=%" PRIu64 " person_states[first=%" PRIu64
	       " tracking=%" PRIu64 " lost=%" PRIu64 " disappear=%" PRIu64 "]"
	       " release_callbacks=%" PRIu64 " release_entries=%" PRIu64
	       " released_frames=%" PRIu64 " release_unmatched=%" PRIu64
	       " release_duplicates=%" PRIu64 " release_mismatches=%" PRIu64
	       " release_invalid=%" PRIu64 " channel_mismatches=%" PRIu64
	       " detect_latency_ms[min=%.3f max=%.3f avg=%.3f]"
	       " release_latency_ms[min=%.3f max=%.3f avg=%.3f]\n",
	       options.input_kind == PROBE_INPUT_MP4 ? "mp4-dmabuf" : "dmabuf",
	       state.samples_received, state.samples_rejected, state.pushed,
	       state.push_failures, state.detections, state.detection_errors,
	       state.person_observations, state.person_first_observations,
	       state.person_tracking_observations, state.person_lost_observations,
	       state.person_disappear_observations, state.release_callbacks,
	       state.release_entries, state.released_frames, state.release_unmatched,
	       state.release_duplicates, state.release_mismatches,
	       state.release_invalid_callbacks, state.channel_mismatches,
	       (double)state.detection_latency_min_ns / 1000000.0,
	       (double)state.detection_latency_max_ns / 1000000.0,
	       state.detection_latency_count
		? (double)state.detection_latency_sum_ns /
		      (double)state.detection_latency_count / 1000000.0
		: 0.0,
	       (double)state.release_latency_min_ns / 1000000.0,
	       (double)state.release_latency_max_ns / 1000000.0,
	       state.release_latency_count
		? (double)state.release_latency_sum_ns /
		      (double)state.release_latency_count / 1000000.0
		: 0.0);
	pipeline_cleanup_safe = !handle_initialized ||
		(final_cleanup_safe && release_succeeded && callbacks_complete_locked(&state));
	pthread_mutex_unlock(&state.metrics_lock);

	/* The pipeline remains alive until all SDK-owned refs have been returned.
	 * In an unsafe failure path it is deliberately left referenced until process
	 * exit, because tearing it down could invalidate a buffer still held by SDK. */
	if (pipeline && pipeline_cleanup_safe) {
		if (pipeline_started && stop_pipeline(pipeline, options.timeout_ms) != 0)
			operation_failed = 1;
		if (bus)
			gst_object_unref(bus);
		if (appsink)
			gst_object_unref(appsink);
		gst_object_unref(pipeline);
	} else if (pipeline) {
		fprintf(stderr, "leaving capture pipeline referenced until process exit "
			"because SDK ownership was not proven released\n");
	}
	if (pipeline_cleanup_safe && state.frames) {
		free(state.frames);
		state.frames = NULL;
	}
	if (callbacks_cond_initialized && pipeline_cleanup_safe)
		(void)pthread_cond_destroy(&state.callbacks_cond);
	if (metrics_lock_initialized && pipeline_cleanup_safe)
		pthread_mutex_destroy(&state.metrics_lock);
	return operation_failed ? 1 : 0;
}
