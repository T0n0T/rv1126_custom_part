/*
 * Native V4L2 MMAP/EXPBUF to RockIVA lifecycle probe (test tool only).
 *
 * This executable owns an independent V4L2 queue.  It is not linked into the
 * production media_engine and must not be used against its active mainpath by
 * accident.  The capture node is configured as single-physical-plane NV12,
 * exported fds are kept open while RockIVA owns the corresponding frames, and
 * a capture buffer is requeued only after its matching release callback.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"
#include "mainpath_guard.h"

#define V4L2_CAPTURE_TYPE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define V4L2_MEMORY_TYPE V4L2_MEMORY_MMAP
#define DEFAULT_MODEL ROCKIVA_DET_MODEL_PFP
#define DEFAULT_CHANNEL 0U
#define DEFAULT_CORE_MASK 0U
#define DEFAULT_BUFFERS 4U
#define DEFAULT_TIMEOUT_MS 5000
#define DEFAULT_MIN_PERSON_OBSERVATIONS 0U
#define DEFAULT_MIN_TRACKING_OBSERVATIONS 0U
#define MAX_QUEUE_BUFFERS 64U
#define MAX_CAPTURE_FRAMES 10000U
#define MAX_VERSION 128U

struct probe_options {
	const char *device;
	const char *model_path;
	unsigned int width;
	unsigned int height;
	unsigned int frames;
	unsigned int buffers;
	unsigned int channel_id;
	unsigned int core_mask;
	RockIvaDetModel model;
	int timeout_ms;
	unsigned int min_person_observations;
	unsigned int min_tracking_observations;
	int allow_mainpath;
};

struct capture_buffer {
	struct v4l2_plane plane;
	void *map_addr;
	size_t map_length;
	int export_fd;
	int queued;
	int requeue_in_progress;
	uint32_t owner_frame_id;
};

enum frame_state {
	FRAME_UNUSED = 0,
	FRAME_PENDING,
	FRAME_ACCEPTED,
	FRAME_RELEASED,
	FRAME_REJECTED,
};

struct probe_frame {
	uint32_t frame_id;
	unsigned int buffer_index;
	uint32_t sequence;
	uint32_t bytesused;
	uint64_t submitted_ns;
	enum frame_state state;
	int push_returned;
	int detection_completed;
	int release_seen;
	int release_valid;
	int release_accounted;
	int requeued;
};

struct probe_state {
	pthread_mutex_t metrics_lock;
	pthread_cond_t callbacks_cond;
	int callbacks_cond_initialized;
	uint32_t channel_id;
	struct capture_buffer *buffers;
	unsigned int buffer_count;
	struct probe_frame *frames;
	size_t frame_count;
	uint64_t captures;
	uint64_t qbufs;
	uint64_t qbuf_failures;
	uint64_t sequence_errors;
	uint64_t capture_errors;
	uint64_t pushed;
	uint64_t push_failures;
	uint64_t accepted_frames;
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
	uint64_t detection_latency_count;
	uint64_t detection_latency_sum_ns;
	uint64_t detection_latency_min_ns;
	uint64_t detection_latency_max_ns;
	uint64_t release_latency_count;
	uint64_t release_latency_sum_ns;
	uint64_t release_latency_min_ns;
	uint64_t release_latency_max_ns;
};

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

static void update_latency(uint64_t submitted, uint64_t now, uint64_t *count,
				   uint64_t *sum_ns, uint64_t *min_ns,
				   uint64_t *max_ns, const char *label)
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

static struct probe_frame *find_frame_locked(struct probe_state *state,
						     uint32_t frame_id)
{
	if (frame_id == 0 || frame_id >= state->frame_count)
		return NULL;
	return &state->frames[frame_id];
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
	return state->detection_completed_frames == state->accepted_frames &&
	       state->released_frames == state->accepted_frames &&
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
			printf("callback_completion status=complete accepted=%" PRIu64
			       " detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
			       state->accepted_frames, state->detection_completed_frames,
			       state->released_frames);
			pthread_mutex_unlock(&state->metrics_lock);
			return 0;
		}
		if (callbacks_have_errors_locked(state)) {
			fprintf(stderr, "callback_completion failed: accepted=%" PRIu64
				" detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
				state->accepted_frames, state->detection_completed_frames,
				state->released_frames);
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
		wait_result = pthread_cond_timedwait(&state->callbacks_cond,
							    &state->metrics_lock, &deadline);
		if (wait_result == ETIMEDOUT) {
			fprintf(stderr, "callback_completion timeout: accepted=%" PRIu64
				" detection_completed=%" PRIu64 " released=%" PRIu64 "\n",
				state->accepted_frames, state->detection_completed_frames,
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
	       " frame_id=%" PRIu32 " rect=%d,%d-%d,%d timestamp=%lu\n",
	       object->objId, object->state, object->type, object->score,
	       object->frameId, object->rect.topLeft.x, object->rect.topLeft.y,
	       object->rect.bottomRight.x, object->rect.bottomRight.y,
	       object->timestamp);
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
	       " objects=%" PRIu32 "\n", status, result ? result->frameId : 0,
	       result ? result->channelId : 0, result ? result->objNum : 0);
	if (!result || status != ROCKIVA_SUCCESS) {
		signal_callbacks_locked(state);
		pthread_mutex_unlock(&state->metrics_lock);
		return;
	}
	if (result->channelId != state->channel_id)
		state->channel_mismatches++;
	record = find_frame_locked(state, result->frameId);
	if (!record || record->state == FRAME_UNUSED || record->state == FRAME_REJECTED) {
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
	uint32_t count;
	uint32_t i;

	if (!state) {
		fprintf(stderr, "release callback received NULL userdata\n");
		return;
	}
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
		struct probe_frame *record = find_frame_locked(state, released->frameId);
		uint64_t now = monotonic_ns();
		uint64_t submitted = record ? record->submitted_ns : 0;

		printf("  released frame_id=%" PRIu32 " buffer=%u fd=%" PRId32
		       " data=%p phy=%p\n", released->frameId,
		       record ? record->buffer_index : UINT_MAX, released->dataFd,
		       (void *)released->dataAddr, (void *)released->dataPhyAddr);
		if (!record || record->state == FRAME_UNUSED ||
		    record->state == FRAME_REJECTED) {
			state->release_unmatched++;
			continue;
		}
		if (record->release_accounted) {
			state->release_duplicates++;
			continue;
		}
		record->release_seen = 1;
		if (released->channelId != state->channel_id ||
		    released->dataFd != state->buffers[record->buffer_index].export_fd ||
		    released->dataAddr != NULL || released->dataPhyAddr != NULL) {
			state->release_mismatches++;
			state->release_unmatched++;
			fprintf(stderr, "release mismatch frame_id=%" PRIu32
				" expected_fd=%d actual_fd=%" PRId32 "\n",
				released->frameId,
				state->buffers[record->buffer_index].export_fd,
				released->dataFd);
			continue;
		}
		record->release_valid = 1;
		record->release_accounted = 1;
		state->released_frames++;
		update_latency(submitted, now, &state->release_latency_count,
		       &state->release_latency_sum_ns,
		       &state->release_latency_min_ns,
		       &state->release_latency_max_ns, "release");
	}
	signal_callbacks_locked(state);
	pthread_mutex_unlock(&state->metrics_lock);
}

static int parse_uint(const char *value, unsigned int maximum,
				      unsigned int *out)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0' || parsed > maximum)
		return -1;
	*out = (unsigned int)parsed;
	return 0;
}

static int parse_positive_uint(const char *value, unsigned int maximum,
				       unsigned int *out)
{
	if (parse_uint(value, maximum, out) != 0 || *out == 0)
		return -1;
	return 0;
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

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s --device PATH --model-path DIR --width N "
		"--height N --frames N [options]\n", program);
	fprintf(stderr, "  --device PATH       V4L2 node (required)\n");
	fprintf(stderr, "  --allow-mainpath    explicitly permit /dev/video24\n");
	fprintf(stderr, "  --model-path DIR    RockIVA model directory (required)\n");
	fprintf(stderr, "  --width N           requested NV12 width (required)\n");
	fprintf(stderr, "  --height N          requested NV12 height (required)\n");
	fprintf(stderr, "  --frames N          captured frame count (required)\n");
	fprintf(stderr, "  --buffers N         V4L2 queue depth (default: %u)\n",
		DEFAULT_BUFFERS);
	fprintf(stderr, "  --model pfp|cls8|person (default: pfp)\n");
	fprintf(stderr, "  --channel N          RockIVA channel (default: %u)\n",
		DEFAULT_CHANNEL);
	fprintf(stderr, "  --core-mask MASK     RockIVA core mask (default: 0x%x)\n",
		DEFAULT_CORE_MASK);
	fprintf(stderr, "  --timeout-ms N       per-frame/callback timeout (default: %d)\n",
		DEFAULT_TIMEOUT_MS);
	fprintf(stderr, "  --min-person N       minimum person observations (default: %u)\n",
		DEFAULT_MIN_PERSON_OBSERVATIONS);
	fprintf(stderr, "  --min-tracking N     minimum TRACKING observations (default: %u)\n",
		DEFAULT_MIN_TRACKING_OBSERVATIONS);
}

static int parse_options(int argc, char **argv, struct probe_options *options)
{
	static const struct option long_options[] = {
		{"device", required_argument, NULL, 'd'},
		{"allow-mainpath", no_argument, NULL, 'a'},
		{"model-path", required_argument, NULL, 'm'},
		{"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'h'},
		{"frames", required_argument, NULL, 'n'},
		{"buffers", required_argument, NULL, 'b'},
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
	int have_width = 0;
	int have_height = 0;
	int have_frames = 0;

	memset(options, 0, sizeof(*options));
	options->buffers = DEFAULT_BUFFERS;
	options->model = DEFAULT_MODEL;
	options->channel_id = DEFAULT_CHANNEL;
	options->core_mask = DEFAULT_CORE_MASK;
	options->timeout_ms = DEFAULT_TIMEOUT_MS;
	options->min_person_observations = DEFAULT_MIN_PERSON_OBSERVATIONS;
	options->min_tracking_observations = DEFAULT_MIN_TRACKING_OBSERVATIONS;
	while ((option = getopt_long(argc, argv, "dam:w:h:n:b:M:c:k:t:p:r:H",
					long_options, NULL)) != -1) {
		switch (option) {
		case 'd':
			options->device = optarg;
			break;
		case 'a':
			options->allow_mainpath = 1;
			break;
		case 'm':
			options->model_path = optarg;
			break;
		case 'w':
			if (parse_positive_uint(optarg, UINT16_MAX, &options->width) != 0)
				return -1;
			have_width = 1;
			break;
		case 'h':
			if (parse_positive_uint(optarg, UINT16_MAX, &options->height) != 0)
				return -1;
			have_height = 1;
			break;
		case 'n':
			if (parse_positive_uint(optarg, MAX_CAPTURE_FRAMES, &options->frames) != 0)
				return -1;
			have_frames = 1;
			break;
		case 'b':
			if (parse_positive_uint(optarg, MAX_QUEUE_BUFFERS, &options->buffers) != 0)
				return -1;
			break;
		case 'M':
			if (parse_model(optarg, &options->model) != 0)
				return -1;
			break;
		case 'c':
			if (parse_uint(optarg, UINT32_MAX, &options->channel_id) != 0)
				return -1;
			break;
		case 'k':
			if (parse_uint(optarg, UINT32_MAX, &options->core_mask) != 0)
				return -1;
			break;
		case 't': {
			unsigned int timeout;

			if (parse_positive_uint(optarg, INT32_MAX, &timeout) != 0)
				return -1;
			options->timeout_ms = (int)timeout;
			break;
		}
		case 'p':
			if (parse_uint(optarg, UINT32_MAX, &options->min_person_observations) != 0)
				return -1;
			break;
		case 'r':
			if (parse_uint(optarg, UINT32_MAX, &options->min_tracking_observations) != 0)
				return -1;
			break;
		case 'H':
			usage(argv[0]);
			return 1;
		default:
			return -1;
		}
	}
	if (optind != argc || !options->device || !options->device[0] ||
	    !options->model_path || !options->model_path[0] || !have_width ||
	    !have_height || !have_frames || options->width % 2 != 0 ||
	    options->height % 2 != 0 || options->frames == UINT32_MAX) {
		return -1;
	}
	if (rockiva_probe_is_mainpath(options->device, ROCKIVA_PROBE_MAINPATH) &&
	    !options->allow_mainpath) {
		fprintf(stderr, "refusing production mainpath %s; pass --allow-mainpath "
			"only for an explicitly approved experiment\n", options->device);
		return -1;
	}
	return 0;
}

static int ioctl_checked(int fd, unsigned long request, void *arg,
				 const char *name)
{
	int result;

	do {
		result = ioctl(fd, request, arg);
	} while (result < 0 && errno == EINTR);
	if (result < 0) {
		int saved_errno = errno;

		fprintf(stderr, "%s failed: errno=%d (%s)\n", name, saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int close_checked(int *fd, const char *name)
{
	int result;
	int saved_errno;

	if (!fd || *fd < 0)
		return 0;
	result = close(*fd);
	saved_errno = errno;
	*fd = -1;
	if (result < 0) {
		fprintf(stderr, "%s failed: errno=%d (%s)\n", name, saved_errno,
			strerror(saved_errno));
		return -1;
	}
	return 0;
}

static int query_capabilities(int fd)
{
	struct v4l2_capability capability;
	__u32 device_caps;

	memset(&capability, 0, sizeof(capability));
	if (ioctl_checked(fd, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP") != 0)
		return -1;
	device_caps = capability.capabilities;
	if (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
		device_caps = capability.device_caps;
	printf("querycap driver=%s card=%s bus=%s version=0x%08x "
	       "capabilities=0x%08x device_caps=0x%08x\n", capability.driver,
	       capability.card, capability.bus_info, capability.version,
	       capability.capabilities, device_caps);
	if (!(device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
		fprintf(stderr, "device lacks V4L2_CAP_VIDEO_CAPTURE_MPLANE\n");
		return -1;
	}
	if (!(device_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "device lacks V4L2_CAP_STREAMING\n");
		return -1;
	}
	return 0;
}

static int get_current_format(int fd, struct v4l2_format *format)
{
	memset(format, 0, sizeof(*format));
	format->type = V4L2_CAPTURE_TYPE;
	if (ioctl_checked(fd, VIDIOC_G_FMT, format, "VIDIOC_G_FMT") != 0)
		return -1;
	printf("format_before width=%u height=%u fourcc=0x%08x field=%u planes=%u\n",
	       format->fmt.pix_mp.width, format->fmt.pix_mp.height,
	       format->fmt.pix_mp.pixelformat, format->fmt.pix_mp.field,
	       format->fmt.pix_mp.num_planes);
	return 0;
}

static void fourcc_string(__u32 fourcc, char out[5])
{
	out[0] = (char)(fourcc & 0xffU);
	out[1] = (char)((fourcc >> 8) & 0xffU);
	out[2] = (char)((fourcc >> 16) & 0xffU);
	out[3] = (char)((fourcc >> 24) & 0xffU);
	out[4] = '\0';
}

struct image_layout {
	uint16_t width;
	uint16_t height;
	uint16_t wstride;
	uint16_t hstride;
	uint32_t sizeimage;
};

static int negotiate_format(int fd, const struct probe_options *options,
				    struct v4l2_format *format,
				    struct image_layout *layout)
{
	char actual_fourcc[5];
	int is_nv12;
	uint64_t line_count;
	uint64_t hstride;
	uint64_t expected_size;
	const struct v4l2_plane_pix_format *plane;

	memset(format, 0, sizeof(*format));
	format->type = V4L2_CAPTURE_TYPE;
	format->fmt.pix_mp.width = options->width;
	format->fmt.pix_mp.height = options->height;
	format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	format->fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl_checked(fd, VIDIOC_S_FMT, format, "VIDIOC_S_FMT") != 0)
		return -1;
	fourcc_string(format->fmt.pix_mp.pixelformat, actual_fourcc);
	is_nv12 = format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12 ||
		format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M;
	printf("format_requested=%ux%u NV12 negotiated=%ux%u fourcc=%s "
	       "field=%u physical_planes=%u flags=0x%08x\n", options->width,
	       options->height, format->fmt.pix_mp.width, format->fmt.pix_mp.height,
	       actual_fourcc, format->fmt.pix_mp.field,
	       format->fmt.pix_mp.num_planes, format->fmt.pix_mp.flags);
	if (format->fmt.pix_mp.width != options->width ||
	    format->fmt.pix_mp.height != options->height || !is_nv12 ||
	    format->fmt.pix_mp.field != V4L2_FIELD_NONE) {
		fprintf(stderr, "VIDIOC_S_FMT did not retain progressive NV12/NV12M\n");
		return -1;
	}
	if (format->fmt.pix_mp.num_planes != 1) {
		fprintf(stderr, "rejecting fourcc=%s with physical_planes=%u: RockIVA "
			"has only one dataFd\n", actual_fourcc,
			format->fmt.pix_mp.num_planes);
		return -1;
	}
	plane = &format->fmt.pix_mp.plane_fmt[0];
	if (plane->bytesperline == 0 || plane->bytesperline < format->fmt.pix_mp.width ||
	    plane->bytesperline > UINT16_MAX || plane->sizeimage == 0) {
		fprintf(stderr, "invalid negotiated single-plane stride/size: "
			"bytesperline=%u sizeimage=%u\n", plane->bytesperline,
			plane->sizeimage);
		return -1;
	}
	if (plane->sizeimage % plane->bytesperline != 0) {
		fprintf(stderr, "cannot derive exact NV12 hstride: sizeimage=%u "
			"bytesperline=%u\n", plane->sizeimage, plane->bytesperline);
		return -1;
	}
	line_count = plane->sizeimage / plane->bytesperline;
	if (line_count == 0 || line_count % 3U != 0)
		goto layout_invalid;
	hstride = line_count * 2U / 3U;
	if (hstride < format->fmt.pix_mp.height || hstride > UINT16_MAX)
		goto layout_invalid;
	if (hstride > UINT64_MAX / plane->bytesperline)
		goto layout_invalid;
	expected_size = hstride * plane->bytesperline;
	if (expected_size > UINT64_MAX / 3U || expected_size * 3U / 2U != plane->sizeimage)
		goto layout_invalid;
	memset(layout, 0, sizeof(*layout));
	layout->width = (uint16_t)format->fmt.pix_mp.width;
	layout->height = (uint16_t)format->fmt.pix_mp.height;
	layout->wstride = (uint16_t)plane->bytesperline;
	layout->hstride = (uint16_t)hstride;
	layout->sizeimage = plane->sizeimage;
	printf("layout width=%u height=%u wstride=%u hstride=%u sizeimage=%u "
	       "single_fd_candidate=1\n", layout->width, layout->height,
	       layout->wstride, layout->hstride, layout->sizeimage);
	return 0;

layout_invalid:
	fprintf(stderr, "cannot derive safe NV12 hstride from sizeimage=%u "
		"bytesperline=%u height=%u\n", plane->sizeimage,
		plane->bytesperline, format->fmt.pix_mp.height);
	return -1;
}

static int request_buffers(int fd, unsigned int requested,
				   unsigned int *granted, int *queue_allocated)
{
	struct v4l2_requestbuffers request;

	*granted = 0;
	*queue_allocated = 0;
	memset(&request, 0, sizeof(request));
	request.type = V4L2_CAPTURE_TYPE;
	request.memory = V4L2_MEMORY_TYPE;
	request.count = requested;
	if (ioctl_checked(fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS(MMAP)") != 0)
		return -1;
	*queue_allocated = 1;
	*granted = request.count;
	if (request.count == 0 || request.count > MAX_QUEUE_BUFFERS) {
		fprintf(stderr, "VIDIOC_REQBUFS returned unexpected count=%u\n",
			request.count);
		*granted = 0;
		return -1;
	}
	printf("buffers requested=%u granted=%u memory=MMAP\n", requested,
	       *granted);
	return 0;
}

static int query_and_prepare_buffer(int fd, unsigned int index,
					struct capture_buffer *capture,
					const struct image_layout *layout)
{
	struct v4l2_buffer buffer;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];
	struct v4l2_exportbuffer export_buffer;
	struct stat status;

	memset(capture, 0, sizeof(*capture));
	capture->export_fd = -1;
	memset(&buffer, 0, sizeof(buffer));
	memset(plane, 0, sizeof(plane));
	buffer.type = V4L2_CAPTURE_TYPE;
	buffer.memory = V4L2_MEMORY_TYPE;
	buffer.index = index;
	buffer.m.planes = plane;
	buffer.length = VIDEO_MAX_PLANES;
	if (ioctl_checked(fd, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF") != 0)
		return -1;
	if (buffer.length != 1 || plane[0].length < layout->sizeimage ||
	    plane[0].length == 0 || plane[0].data_offset != 0) {
		fprintf(stderr, "reject buffer=%u physical_planes=%u length=%u "
			"sizeimage=%u data_offset=%u\n", index, buffer.length,
			plane[0].length, layout->sizeimage, plane[0].data_offset);
		return -1;
	}
	capture->plane = plane[0];
	capture->map_length = plane[0].length;
	capture->map_addr = mmap(NULL, capture->map_length, PROT_READ | PROT_WRITE,
					 MAP_SHARED, fd, plane[0].m.mem_offset);
	if (capture->map_addr == MAP_FAILED) {
		int saved_errno = errno;

		capture->map_addr = NULL;
		fprintf(stderr, "mmap buffer=%u failed: errno=%d (%s)\n", index,
			saved_errno, strerror(saved_errno));
		return -1;
	}
	printf("mmap buffer=%u length=%u offset=0x%08x addr=%p\n", index,
	       plane[0].length, plane[0].m.mem_offset, capture->map_addr);
	memset(&export_buffer, 0, sizeof(export_buffer));
	export_buffer.type = V4L2_CAPTURE_TYPE;
	export_buffer.index = index;
	export_buffer.plane = 0;
	export_buffer.flags = O_CLOEXEC | O_RDWR;
	if (ioctl_checked(fd, VIDIOC_EXPBUF, &export_buffer, "VIDIOC_EXPBUF") != 0)
		return -1;
	capture->export_fd = export_buffer.fd;
	if (capture->export_fd < 0 || fcntl(capture->export_fd, F_GETFD) < 0 ||
	    fstat(capture->export_fd, &status) < 0) {
		int saved_errno = errno;

		fprintf(stderr, "export fd validation buffer=%u failed: fd=%d "
			"errno=%d (%s)\n", index, capture->export_fd, saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	printf("export buffer=%u plane=0 fd=%d valid=1 mode=0%o retained=1\n",
	       index, capture->export_fd, status.st_mode & 07777);
	return 0;
}

static int qbuf_capture(int fd, const struct capture_buffer *capture,
				       unsigned int index)
{
	struct v4l2_buffer buffer;
	struct v4l2_plane plane[VIDEO_MAX_PLANES];

	memset(&buffer, 0, sizeof(buffer));
	memset(plane, 0, sizeof(plane));
	buffer.type = V4L2_CAPTURE_TYPE;
	buffer.memory = V4L2_MEMORY_TYPE;
	buffer.index = index;
	buffer.m.planes = plane;
	buffer.length = 1;
	plane[0].length = capture->plane.length;
	if (ioctl_checked(fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF") != 0)
		return -1;
	return 0;
}

static int dqbuf_capture(int fd, unsigned int expected_planes,
				 struct v4l2_buffer *buffer, struct v4l2_plane *plane)
{
	memset(buffer, 0, sizeof(*buffer));
	memset(plane, 0, sizeof(*plane) * VIDEO_MAX_PLANES);
	buffer->type = V4L2_CAPTURE_TYPE;
	buffer->memory = V4L2_MEMORY_TYPE;
	buffer->m.planes = plane;
	buffer->length = VIDEO_MAX_PLANES;
	if (ioctl_checked(fd, VIDIOC_DQBUF, buffer, "VIDIOC_DQBUF") != 0)
		return -1;
	if (buffer->length != expected_planes || buffer->length != 1)
		return -1;
	return 0;
}

static int stream_on(int fd)
{
	enum v4l2_buf_type type = V4L2_CAPTURE_TYPE;

	return ioctl_checked(fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
}

static int stream_off(int fd, int stream_started)
{
	enum v4l2_buf_type type = V4L2_CAPTURE_TYPE;

	if (ioctl_checked(fd, VIDIOC_STREAMOFF, &type, "VIDIOC_STREAMOFF") == 0)
		return 0;
	if (!stream_started && errno == EINVAL) {
		fprintf(stderr, "VIDIOC_STREAMOFF returned EINVAL before STREAMON; "
			"queue is already stopped\n");
		return 0;
	}
	return -1;
}

static int release_queue(int fd, struct capture_buffer *buffers,
				 unsigned int buffer_count, int stream_started)
{
	struct v4l2_requestbuffers request;
	unsigned int i;
	int result = 0;

	if (stream_off(fd, stream_started) != 0) {
		/* Do not unmap, release, or close DMA-BUFs while the driver may still
		 * be streaming.  The caller leaves the queue alive until process exit. */
		fprintf(stderr, "deferring V4L2 queue teardown after STREAMOFF failure\n");
		return -1;
	}
	for (i = 0; i < buffer_count; i++) {
		if (!buffers)
			break;
		if (buffers[i].map_addr) {
			if (munmap(buffers[i].map_addr, buffers[i].map_length) != 0) {
				int saved_errno = errno;

				fprintf(stderr, "munmap buffer=%u failed: errno=%d (%s)\n",
					i, saved_errno, strerror(saved_errno));
				result = -1;
			}
			buffers[i].map_addr = NULL;
		}
	}
	memset(&request, 0, sizeof(request));
	request.type = V4L2_CAPTURE_TYPE;
	request.memory = V4L2_MEMORY_TYPE;
	request.count = 0;
	if (ioctl_checked(fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS(0)") != 0)
		result = -1;
	for (i = 0; i < buffer_count; i++) {
		if (!buffers)
			break;
		if (close_checked(&buffers[i].export_fd,
				  "close(retained DMA-BUF fd)") != 0)
			result = -1;
	}
	return result;
}

static int restore_format(int fd, const struct v4l2_format *original)
{
	struct v4l2_format format;

	if (!original)
		return -1;
	format = *original;
	if (ioctl_checked(fd, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT(restore)") != 0)
		return -1;
	if (format.fmt.pix_mp.width != original->fmt.pix_mp.width ||
	    format.fmt.pix_mp.height != original->fmt.pix_mp.height ||
	    format.fmt.pix_mp.pixelformat != original->fmt.pix_mp.pixelformat ||
	    format.fmt.pix_mp.field != original->fmt.pix_mp.field ||
	    format.fmt.pix_mp.num_planes != original->fmt.pix_mp.num_planes) {
		fprintf(stderr, "VIDIOC_S_FMT(restore) did not retain original format\n");
		return -1;
	}
	printf("format_restore=ok width=%u height=%u fourcc=0x%08x field=%u "
	       "planes=%u\n", format.fmt.pix_mp.width, format.fmt.pix_mp.height,
	       format.fmt.pix_mp.pixelformat, format.fmt.pix_mp.field,
	       format.fmt.pix_mp.num_planes);
	return 0;
}

static int requeue_released_buffers(int fd, struct probe_state *state)
{
	unsigned int i;
	int result = 0;

	for (i = 0; i < state->buffer_count; i++) {
		struct capture_buffer *capture = &state->buffers[i];
		struct probe_frame *record;
		uint32_t frame_id;

		pthread_mutex_lock(&state->metrics_lock);
		if (capture->queued || capture->requeue_in_progress ||
		    capture->owner_frame_id == 0) {
			pthread_mutex_unlock(&state->metrics_lock);
			continue;
		}
		record = find_frame_locked(state, capture->owner_frame_id);
		if (!record || !record->release_valid || record->requeued) {
			pthread_mutex_unlock(&state->metrics_lock);
			continue;
		}
		capture->requeue_in_progress = 1;
		frame_id = record->frame_id;
		pthread_mutex_unlock(&state->metrics_lock);

		if (qbuf_capture(fd, capture, i) != 0) {
			pthread_mutex_lock(&state->metrics_lock);
			capture->requeue_in_progress = 0;
			state->qbuf_failures++;
			signal_callbacks_locked(state);
			pthread_mutex_unlock(&state->metrics_lock);
			fprintf(stderr, "requeue failed buffer=%u frame_id=%" PRIu32 "\n",
				i, frame_id);
			result = -1;
			continue;
		}
		pthread_mutex_lock(&state->metrics_lock);
		capture->requeue_in_progress = 0;
		capture->queued = 1;
		capture->owner_frame_id = 0;
		record->requeued = 1;
		state->qbufs++;
		signal_callbacks_locked(state);
		pthread_mutex_unlock(&state->metrics_lock);
		printf("requeue buffer=%u frame_id=%" PRIu32 " fd=%d\n", i, frame_id,
		       capture->export_fd);
	}
	return result;
}

static int has_queued_capture(struct probe_state *state)
{
	unsigned int i;
	int result = 0;

	pthread_mutex_lock(&state->metrics_lock);
	for (i = 0; i < state->buffer_count; i++) {
		if (state->buffers[i].queued) {
			result = 1;
			break;
		}
	}
	pthread_mutex_unlock(&state->metrics_lock);
	return result;
}

static int has_requeueable_buffer_locked(struct probe_state *state)
{
	unsigned int i;

	for (i = 0; i < state->buffer_count; i++) {
		const struct capture_buffer *capture = &state->buffers[i];
		const struct probe_frame *record;

		if (capture->queued || capture->requeue_in_progress ||
		    capture->owner_frame_id == 0)
			continue;
		record = find_frame_locked(state, capture->owner_frame_id);
		if (record && record->release_valid && !record->requeued)
			return 1;
	}
	return 0;
}

static int wait_for_released_buffer(struct probe_state *state, int timeout_ms)
{
	struct timespec deadline;
	int wait_result;

	if (!state->callbacks_cond_initialized || make_deadline(&deadline, timeout_ms) != 0) {
		fprintf(stderr, "cannot establish bounded release wait\n");
		return -1;
	}
	pthread_mutex_lock(&state->metrics_lock);
	for (;;) {
		if (has_requeueable_buffer_locked(state)) {
			pthread_mutex_unlock(&state->metrics_lock);
			return 0;
		}
		if (callbacks_have_errors_locked(state)) {
			fprintf(stderr, "release wait failed: callback accounting error\n");
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
		wait_result = pthread_cond_timedwait(&state->callbacks_cond,
							    &state->metrics_lock, &deadline);
		if (wait_result == ETIMEDOUT) {
			fprintf(stderr, "release wait timeout: no requeueable capture buffer\n");
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
		if (wait_result != 0) {
			fprintf(stderr, "release wait failed: %s\n", strerror(wait_result));
			pthread_mutex_unlock(&state->metrics_lock);
			return -1;
		}
	}
}

static int wait_for_capture_event(int fd, int timeout_ms)
{
	struct pollfd poll_fd;
	int result;

	memset(&poll_fd, 0, sizeof(poll_fd));
	poll_fd.fd = fd;
	poll_fd.events = POLLIN | POLLPRI;
	do {
		result = poll(&poll_fd, 1, timeout_ms);
	} while (result < 0 && errno == EINTR);
	if (result < 0) {
		int saved_errno = errno;

		fprintf(stderr, "poll failed: errno=%d (%s)\n", saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	if (result == 0)
		return 1;
	if (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
		fprintf(stderr, "poll returned revents=0x%x\n", poll_fd.revents);
		return -1;
	}
	return 0;
}

static enum frame_state mark_push_failure_locked(struct probe_state *state,
						 uint32_t frame_id)
{
	struct probe_frame *record = find_frame_locked(state, frame_id);

	if (!record || record->state != FRAME_PENDING)
		return FRAME_REJECTED;
	record->push_returned = 1;
	if (record->detection_completed || record->release_seen) {
		record->state = FRAME_ACCEPTED;
		state->accepted_frames++;
		return FRAME_ACCEPTED;
	}
	record->state = FRAME_REJECTED;
	state->buffers[record->buffer_index].owner_frame_id = 0;
	return FRAME_REJECTED;
}

static int wait_finish(RockIvaHandle handle, int timeout_ms, const char *label)
{
	RockIvaRetCode ret = ROCKIVA_WaitFinish(handle, -1, timeout_ms);

	if (ret == ROCKIVA_RET_SUCCESS) {
		printf("%s ret=%d status=success\n", label, ret);
		return 0;
	}
	if (ret == ROCKIVA_RET_UNSUPPORTED) {
		printf("%s ret=%d status=unsupported capability fallback\n", label, ret);
		return 1;
	}
	printf("%s ret=%d status=failure\n", label, ret);
	return -1;
}

static int drain_callbacks(struct probe_state *state, int timeout_ms,
				   const char *label)
{
	printf("%s: bounded callback completion fallback\n", label);
	return wait_for_callback_completion(state, timeout_ms);
}

int main(int argc, char **argv)
{
	struct probe_options options;
	struct v4l2_format original_format;
	struct v4l2_format negotiated_format;
	struct image_layout layout;
	struct capture_buffer *buffers = NULL;
	static struct probe_state state;
	RockIvaInitParam init_params;
	RockIvaDetTaskParams det_params;
	RockIvaHandle handle = NULL;
	RockIvaRetCode ret;
	int fd = -1;
	char version[MAX_VERSION] = {0};
	unsigned int buffer_count = 0;
	unsigned int buffer;
	unsigned int captured = 0;
	uint32_t expected_sequence = 0;
	int parse_result;
	int metrics_lock_initialized = 0;
	int callbacks_cond_initialized = 0;
	int cond_attr_initialized = 0;
	int handle_initialized = 0;
	int detect_initialized = 0;
	int release_callback_initialized = 0;
	int queue_allocated = 0;
	int stream_started = 0;
	int format_restore_required = 0;
	int operation_failed = 0;
	int callback_complete = 0;
	int final_cleanup_safe = 0;
	int sdk_released = 0;
	int v4l2_cleanup_safe = 1;
	int opened_mainpath;
	int result = 1;
	pthread_condattr_t cond_attr;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0)
		return parse_result > 0 ? 0 : 2;
	memset(&state, 0, sizeof(state));
	if (pthread_mutex_init(&state.metrics_lock, NULL) != 0) {
		fprintf(stderr, "cannot initialize metrics lock\n");
		return 2;
	}
	metrics_lock_initialized = 1;
	if (pthread_condattr_init(&cond_attr) == 0) {
		cond_attr_initialized = 1;
		if (pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC) == 0 &&
		    pthread_cond_init(&state.callbacks_cond, &cond_attr) == 0) {
			callbacks_cond_initialized = 1;
			state.callbacks_cond_initialized = 1;
		}
	}
	if (cond_attr_initialized)
		(void)pthread_condattr_destroy(&cond_attr);
	if (!callbacks_cond_initialized) {
		fprintf(stderr, "cannot initialize callback condition variable\n");
		operation_failed = 1;
		goto cleanup;
	}
	state.channel_id = options.channel_id;
	state.frame_count = (size_t)options.frames + 1U;
	if (state.frame_count <= options.frames ||
	    state.frame_count > SIZE_MAX / sizeof(*state.frames)) {
		fprintf(stderr, "frame table size overflow\n");
		operation_failed = 1;
		goto cleanup;
	}
	state.frames = calloc(state.frame_count, sizeof(*state.frames));
	if (!state.frames) {
		fprintf(stderr, "cannot allocate frame table\n");
		operation_failed = 1;
		goto cleanup;
	}
	printf("probe mode=v4l2-rockiva device=%s model=%s model_path=%s width=%u "
	       "height=%u frames=%u requested_buffers=%u channel=%u core_mask=0x%x\n",
	       options.device, model_name(options.model), options.model_path,
	       options.width, options.height, options.frames, options.buffers,
	       options.channel_id, options.core_mask);
	fd = open(options.device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		int saved_errno = errno;

		fprintf(stderr, "open %s failed: errno=%d (%s)\n", options.device,
			saved_errno, strerror(saved_errno));
		operation_failed = 1;
		goto cleanup;
	}
	opened_mainpath = rockiva_probe_fd_is_mainpath(fd,
							ROCKIVA_PROBE_MAINPATH);
	if (opened_mainpath < 0) {
		int saved_errno = errno;

		fprintf(stderr, "cannot verify opened V4L2 device identity: errno=%d (%s)\n",
			saved_errno, strerror(saved_errno));
		errno = saved_errno;
		operation_failed = 1;
		goto cleanup;
	}
	if (opened_mainpath && !options.allow_mainpath) {
		fprintf(stderr,
			"refusing opened production mainpath %s after identity check; pass "
			"--allow-mainpath only for an explicitly approved experiment\n",
			options.device);
		operation_failed = 1;
		goto cleanup;
	}
	if (query_capabilities(fd) != 0 ||
	    get_current_format(fd, &original_format) != 0)
		goto cleanup;
	format_restore_required = 1;
	if (negotiate_format(fd, &options, &negotiated_format, &layout) != 0)
		goto cleanup;
	if (request_buffers(fd, options.buffers, &buffer_count,
			    &queue_allocated) != 0)
		goto cleanup;
	buffers = calloc(buffer_count, sizeof(*buffers));
	if (!buffers) {
		fprintf(stderr, "cannot allocate V4L2 buffer table\n");
		operation_failed = 1;
		goto cleanup;
	}
	for (buffer = 0; buffer < buffer_count; buffer++)
		buffers[buffer].export_fd = -1;
	state.buffers = buffers;
	state.buffer_count = buffer_count;
	for (buffer = 0; buffer < buffer_count; buffer++) {
		if (query_and_prepare_buffer(fd, buffer, &buffers[buffer], &layout) != 0) {
			operation_failed = 1;
			goto cleanup;
		}
	}
	memset(&init_params, 0, sizeof(init_params));
	init_params.logLevel = ROCKIVA_LOG_ERROR;
	ret = snprintf(init_params.modelPath, sizeof(init_params.modelPath), "%s",
		       options.model_path);
	if (ret < 0 || (size_t)ret >= sizeof(init_params.modelPath)) {
		fprintf(stderr, "model path is too long for RockIVA\n");
		operation_failed = 1;
		goto cleanup;
	}
	init_params.coreMask = options.core_mask;
	init_params.channelId = options.channel_id;
	init_params.detModel = options.model;
	init_params.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
	init_params.imageInfo.width = layout.width;
	init_params.imageInfo.height = layout.height;
	init_params.imageInfo.wstride = layout.wstride;
	init_params.imageInfo.hstride = layout.hstride;
	init_params.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
	init_params.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;
	memset(&det_params, 0, sizeof(det_params));
	det_params.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);
	ret = ROCKIVA_GetVersion(MAX_VERSION, version);
	printf("rockiva version ret=%d value=%s\n", ret,
	       version[0] ? version : "(unavailable)");
	if (ret != ROCKIVA_RET_SUCCESS) {
		operation_failed = 1;
		goto cleanup;
	}
	ret = ROCKIVA_Init(&handle, ROCKIVA_MODE_VIDEO, &init_params, &state);
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_Init failed: %d\n", ret);
		operation_failed = 1;
		goto cleanup;
	}
	handle_initialized = 1;
	v4l2_cleanup_safe = 0;
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
	for (buffer = 0; buffer < buffer_count; buffer++) {
		if (qbuf_capture(fd, &buffers[buffer], buffer) != 0) {
			state.qbuf_failures++;
			operation_failed = 1;
			goto sdk_cleanup;
		}
		buffers[buffer].queued = 1;
		state.qbufs++;
	}
	if (stream_on(fd) != 0) {
		operation_failed = 1;
		goto sdk_cleanup;
	}
	stream_started = 1;
	while (captured < options.frames) {
		struct v4l2_buffer dq_buffer;
		struct v4l2_plane dq_planes[VIDEO_MAX_PLANES];
		struct capture_buffer *capture;
		struct probe_frame *record;
		RockIvaImage image;
		uint32_t frame_id = captured + 1U;
		int poll_result;
		int dq_result;

		if (requeue_released_buffers(fd, &state) != 0) {
			operation_failed = 1;
			break;
		}
		if (!has_queued_capture(&state)) {
			if (wait_for_released_buffer(&state, options.timeout_ms) != 0 ||
			    requeue_released_buffers(fd, &state) != 0) {
				operation_failed = 1;
				break;
			}
		}
		poll_result = wait_for_capture_event(fd, options.timeout_ms);
		if (poll_result != 0) {
			if (poll_result > 0)
				fprintf(stderr, "capture poll timeout before frame=%" PRIu32 "\n",
					frame_id);
			operation_failed = 1;
			break;
		}
		dq_result = dqbuf_capture(fd, negotiated_format.fmt.pix_mp.num_planes,
					  &dq_buffer, dq_planes);
		if (dq_result == 0 && dq_planes[0].data_offset != 0) {
			fprintf(stderr, "reject DQBUF index=%u: nonzero data_offset=%u "
				"(expected 0)\n", dq_buffer.index,
				dq_planes[0].data_offset);
			state.capture_errors++;
			operation_failed = 1;
			break;
		}
		if (dq_result != 0 || dq_buffer.index >= buffer_count ||
		    dq_planes[0].length < layout.sizeimage ||
		    dq_planes[0].length > buffers[dq_buffer.index].plane.length ||
		    dq_planes[0].bytesused < layout.sizeimage ||
		    dq_planes[0].bytesused > dq_planes[0].length ||
		    (dq_buffer.flags & V4L2_BUF_FLAG_ERROR)) {
			fprintf(stderr, "reject DQBUF index=%u bytesused=%u length=%u "
				"data_offset=%u flags=0x%x\n",
				dq_result == 0 ? dq_buffer.index : UINT_MAX,
				dq_result == 0 ? dq_planes[0].bytesused : 0,
				dq_result == 0 ? dq_planes[0].length : 0,
				dq_result == 0 ? dq_planes[0].data_offset : 0,
				dq_result == 0 ? dq_buffer.flags : 0);
			state.capture_errors++;
			operation_failed = 1;
			break;
		}
		capture = &buffers[dq_buffer.index];
		pthread_mutex_lock(&state.metrics_lock);
		if (!capture->queued || capture->owner_frame_id != 0) {
			state.capture_errors++;
			pthread_mutex_unlock(&state.metrics_lock);
			fprintf(stderr, "DQBUF ownership invariant failed index=%u queued=%d "
				"owner_frame_id=%" PRIu32 "\n", dq_buffer.index,
				capture->queued, capture->owner_frame_id);
			operation_failed = 1;
			break;
		}
		capture->queued = 0;
		capture->owner_frame_id = frame_id;
		record = &state.frames[frame_id];
		record->frame_id = frame_id;
		record->buffer_index = dq_buffer.index;
		record->sequence = dq_buffer.sequence;
		record->bytesused = dq_planes[0].bytesused;
		record->submitted_ns = monotonic_ns();
		record->state = FRAME_PENDING;
	state.captures++;
	if (captured != 0 && dq_buffer.sequence != expected_sequence + 1U)
		state.sequence_errors++;
	expected_sequence = dq_buffer.sequence;
	pthread_mutex_unlock(&state.metrics_lock);
	memset(&image, 0, sizeof(image));
	image.frameId = frame_id;
	image.channelId = options.channel_id;
	image.info = init_params.imageInfo;
	image.dataAddr = NULL;
	image.dataPhyAddr = NULL;
	image.dataFd = capture->export_fd;
	ret = ROCKIVA_PushFrame(handle, &image, NULL);
	pthread_mutex_lock(&state.metrics_lock);
	record->push_returned = 1;
	if (ret != ROCKIVA_RET_SUCCESS) {
		state.push_failures++;
		(void)mark_push_failure_locked(&state, frame_id);
	} else {
		state.pushed++;
		state.accepted_frames++;
		if (record->state == FRAME_PENDING)
			record->state = FRAME_ACCEPTED;
	}
	signal_callbacks_locked(&state);
	pthread_mutex_unlock(&state.metrics_lock);
	printf("dq frame_id=%" PRIu32 " buffer=%u sequence=%" PRIu32
	       " bytesused=%u fd=%d push_ret=%d\n", frame_id, dq_buffer.index,
	       dq_buffer.sequence, dq_planes[0].bytesused, capture->export_fd, ret);
	if (requeue_released_buffers(fd, &state) != 0 || ret != ROCKIVA_RET_SUCCESS) {
		operation_failed = 1;
		break;
	}
	captured++;
	}

sdk_cleanup:
	if (handle_initialized) {
		int wait_result = wait_finish(handle, options.timeout_ms, "wait_finish");

		if (wait_result < 0)
			operation_failed = 1;
		if (wait_result == 1 || wait_result == 0) {
			if (wait_result == 1)
				wait_result = drain_callbacks(&state, options.timeout_ms,
							      "wait_finish");
			else
				wait_result = wait_for_callback_completion(&state,
								options.timeout_ms);
			if (wait_result != 0)
				operation_failed = 1;
			else
				callback_complete = 1;
		}
		if (callback_complete) {
			if (requeue_released_buffers(fd, &state) != 0)
				operation_failed = 1;
		}
		if (detect_initialized && callback_complete) {
			ret = ROCKIVA_DETECT_Release(handle);
			printf("detect_release ret=%d\n", ret);
			if (ret != ROCKIVA_RET_SUCCESS)
				operation_failed = 1;
		}
		if (callback_complete) {
			int final_wait = wait_finish(handle, options.timeout_ms,
						      "final_wait_finish");

			if (final_wait == 0) {
				final_cleanup_safe = 1;
			} else if (final_wait == 1) {
				if (drain_callbacks(&state, options.timeout_ms,
							    "final_wait_finish") == 0)
					final_cleanup_safe = 1;
				else
					operation_failed = 1;
			} else {
				operation_failed = 1;
			}
		}
		if (final_cleanup_safe) {
			ret = ROCKIVA_Release(handle);
			printf("rockiva_release ret=%d\n", ret);
			if (ret == ROCKIVA_RET_SUCCESS)
				sdk_released = 1;
			else
				operation_failed = 1;
		} else if (callback_complete) {
			fprintf(stderr, "skipping ROCKIVA_Release after incomplete final wait\n");
		}
	}

	if (handle_initialized && (!final_cleanup_safe || !sdk_released)) {
		fprintf(stderr, "deferring V4L2 teardown because RockIVA ownership was "
			"not fully released\n");
	} else {
		v4l2_cleanup_safe = 1;
	}

cleanup:
	if (fd >= 0 && queue_allocated && v4l2_cleanup_safe) {
		if (release_queue(fd, buffers, buffer_count, stream_started) != 0) {
			operation_failed = 1;
			v4l2_cleanup_safe = 0;
		}
	}
	if (fd >= 0 && format_restore_required && v4l2_cleanup_safe &&
	    restore_format(fd, &original_format) != 0)
		operation_failed = 1;
	if (fd >= 0 && v4l2_cleanup_safe &&
	    close_checked(&fd, "close(V4L2 device)") != 0)
		operation_failed = 1;
	if (!v4l2_cleanup_safe && fd >= 0)
		fprintf(stderr, "leaving V4L2 device and retained DMA-BUF fds open until "
			"process exit\n");
	if (metrics_lock_initialized)
		pthread_mutex_lock(&state.metrics_lock);
	if (state.captures != options.frames || state.pushed != options.frames ||
	    state.push_failures != 0 || state.capture_errors != 0 ||
	    state.qbuf_failures != 0 || state.sequence_errors != 0 ||
	    state.accepted_frames != state.pushed ||
	    state.released_frames != state.pushed || state.detection_errors != 0 ||
	    state.detection_frame_errors != 0 || state.detection_unmatched != 0 ||
	    state.detection_duplicates != 0 || state.detection_object_overflows != 0 ||
	    state.release_unmatched != 0 || state.release_duplicates != 0 ||
	    state.release_mismatches != 0 || state.release_invalid_callbacks != 0 ||
	    state.channel_mismatches != 0)
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
	if (operation_failed || !v4l2_cleanup_safe)
		result = 1;
	else
		result = 0;
	printf("summary captures=%" PRIu64 " qbufs=%" PRIu64
	       " qbuf_failures=%" PRIu64 " sequence_errors=%" PRIu64
	       " capture_errors=%" PRIu64 " accepted=%" PRIu64
	       " pushed=%" PRIu64 " push_failures=%" PRIu64
	       " detected=%" PRIu64 " detection_errors=%" PRIu64
	       " detection_frame_errors=%" PRIu64
	       " released=%" PRIu64 " release_unmatched=%" PRIu64
	       " release_duplicates=%" PRIu64 " release_mismatches=%" PRIu64
	       " person=%" PRIu64 " tracking=%" PRIu64
	       " fd_lifecycle=%s t1=%s\n", state.captures, state.qbufs,
	       state.qbuf_failures, state.sequence_errors, state.capture_errors,
	       state.accepted_frames, state.pushed, state.push_failures,
	       state.detections, state.detection_errors, state.detection_frame_errors,
	       state.released_frames, state.release_unmatched,
	       state.release_duplicates, state.release_mismatches,
	       state.person_observations, state.person_tracking_observations,
	       result == 0 ? "complete" : "incomplete",
	       result == 0 ? "candidate_only_record_board_evidence" : "not_claimed");
	if (metrics_lock_initialized)
		pthread_mutex_unlock(&state.metrics_lock);
	if (v4l2_cleanup_safe) {
		free(buffers);
		free(state.frames);
	}
	if (callbacks_cond_initialized && v4l2_cleanup_safe)
		(void)pthread_cond_destroy(&state.callbacks_cond);
	if (metrics_lock_initialized && v4l2_cleanup_safe)
		(void)pthread_mutex_destroy(&state.metrics_lock);
	(void)release_callback_initialized;
	return result;
}
