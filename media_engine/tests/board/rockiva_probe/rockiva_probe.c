/*
 * RockIVA board probe (test tool only).
 *
 * This intentionally exercises a compact CPU-addressed NV12 file first. It
 * does not belong to the media_engine production target and makes no claim
 * about DMA-BUF ownership until the same callback accounting is repeated on
 * a real board with the capture allocator.
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rockiva/rockiva_common.h"
#include "rockiva/rockiva_det_api.h"

#define DEFAULT_WIDTH 640U
#define DEFAULT_HEIGHT 360U
#define DEFAULT_FRAMES 30U
#define DEFAULT_FPS 10U
#define DEFAULT_CHANNEL 0U
#define DEFAULT_CORE_MASK 0U
#define DEFAULT_MODEL ROCKIVA_DET_MODEL_PFP
#define DEFAULT_TIMEOUT_MS 5000
#define DEFAULT_MIN_PERSON_OBSERVATIONS 1U
#define DEFAULT_MIN_TRACKING_OBSERVATIONS 1U
#define MAX_VERSION 128U

struct probe_options {
	const char *model_path;
	const char *input_path;
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
};

enum probe_frame_state {
	PROBE_FRAME_UNUSED = 0,
	PROBE_FRAME_PENDING,
	PROBE_FRAME_ACCEPTED,
	PROBE_FRAME_RELEASED,
	PROBE_FRAME_REJECTED,
};

struct probe_frame {
	uint8_t *buffer;
	uint64_t submitted_ns;
	enum probe_frame_state state;
};

struct probe_state {
	pthread_mutex_t metrics_lock;
	uint32_t channel_id;
	uint64_t pushed;
	uint64_t push_failures;
	uint64_t detections;
	uint64_t detection_errors;
	uint64_t detection_frame_errors;
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

static uint64_t monotonic_ns(void)
{
	struct timespec ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
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
	printf("usage: %s --model-path DIR --input FILE [options]\n", program);
	printf("  --model-path DIR       RockIVA model directory\n");
	printf("  --input FILE           raw tightly-packed NV12 file\n");
	printf("  --model pfp|cls8|person (default: pfp)\n");
	printf("  --width N              frame width (default: %u)\n", DEFAULT_WIDTH);
	printf("  --height N             frame height (default: %u)\n", DEFAULT_HEIGHT);
	printf("  --frames N             frames to push (default: %u)\n", DEFAULT_FRAMES);
	printf("  --fps N                input pacing (default: %u)\n", DEFAULT_FPS);
	printf("  --channel N            RockIVA channel ID (default: %u)\n", DEFAULT_CHANNEL);
	printf("  --core-mask MASK       SDK core mask (default: 0x%x)\n", DEFAULT_CORE_MASK);
	printf("  --timeout-ms N         ROCKIVA_WaitFinish timeout (default: %d)\n", DEFAULT_TIMEOUT_MS);
	printf("  --min-person N         required person observations (default: %u)\n",
	       DEFAULT_MIN_PERSON_OBSERVATIONS);
	printf("  --min-tracking N       required person TRACKING observations (default: %u)\n",
	       DEFAULT_MIN_TRACKING_OBSERVATIONS);
}

static int parse_options(int argc, char **argv, struct probe_options *options)
{
	static const struct option long_options[] = {
		{"model-path", required_argument, NULL, 'm'},
		{"input", required_argument, NULL, 'i'},
		{"model", required_argument, NULL, 'M'},
		{"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'h'},
		{"frames", required_argument, NULL, 'n'},
		{"fps", required_argument, NULL, 'f'},
		{"channel", required_argument, NULL, 'c'},
		{"core-mask", required_argument, NULL, 'k'},
		{"timeout-ms", required_argument, NULL, 't'},
		{"min-person", required_argument, NULL, 'p'},
		{"min-tracking", required_argument, NULL, 'r'},
		{"help", no_argument, NULL, 'H'},
		{NULL, 0, NULL, 0},
	};
	int option;

	memset(options, 0, sizeof(*options));
	options->width = DEFAULT_WIDTH;
	options->height = DEFAULT_HEIGHT;
	options->frames = DEFAULT_FRAMES;
	options->fps = DEFAULT_FPS;
	options->channel_id = DEFAULT_CHANNEL;
	options->core_mask = DEFAULT_CORE_MASK;
	options->model = DEFAULT_MODEL;
	options->timeout_ms = DEFAULT_TIMEOUT_MS;
	options->min_person_observations = DEFAULT_MIN_PERSON_OBSERVATIONS;
	options->min_tracking_observations = DEFAULT_MIN_TRACKING_OBSERVATIONS;

	while ((option = getopt_long(argc, argv, "m:i:M:w:h:n:f:c:k:t:p:r:?", long_options,
				    NULL)) != -1) {
		switch (option) {
		case 'm':
			options->model_path = optarg;
			break;
		case 'i':
			options->input_path = optarg;
			break;
		case 'M':
			if (parse_model(optarg, &options->model) != 0)
				return -1;
			break;
		case 'w':
			if (parse_positive_uint(optarg, &options->width) != 0)
				return -1;
			break;
		case 'h':
			if (parse_positive_uint(optarg, &options->height) != 0)
				return -1;
			break;
		case 'n':
			if (parse_positive_uint(optarg, &options->frames) != 0)
				return -1;
			break;
		case 'f':
			if (parse_positive_uint(optarg, &options->fps) != 0)
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
			return -1;
		default:
			return -1;
		}
	}

	if (optind != argc || !options->model_path || !options->model_path[0] ||
	    !options->input_path || !options->input_path[0] || options->width % 2 != 0 ||
	    options->height % 2 != 0)
		return -1;
	return 0;
}

static void print_object(const RockIvaObjectInfo *object)
{
	printf("  object obj_id=%" PRIu32 " state=%d type=%d score=%" PRIu32
	       " frame_id=%" PRIu32 " rect=%d,%d-%d,%d timestamp=%lu\n",
	       object->objId, object->state, object->type, object->score, object->frameId,
	       object->rect.topLeft.x, object->rect.topLeft.y, object->rect.bottomRight.x,
	       object->rect.bottomRight.y, object->timestamp);
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

static struct probe_frame *find_frame_locked(struct probe_state *state,
						 const RockIvaImage *released)
{
	struct probe_frame *by_id = NULL;
	struct probe_frame *by_pointer = NULL;
	size_t i;

	if (released->frameId < state->frame_count)
		by_id = &state->frames[released->frameId];
	if (released->dataAddr) {
		for (i = 0; i < state->frame_count; i++) {
			if (state->frames[i].buffer == released->dataAddr) {
				by_pointer = &state->frames[i];
				break;
			}
		}
	}
	if (by_id && by_pointer && by_id != by_pointer)
		state->release_mismatches++;

	/* Prefer the frame ID, including a RELEASED record, to classify duplicates. */
	if (by_id && by_id->state != PROBE_FRAME_UNUSED &&
	    by_id->state != PROBE_FRAME_REJECTED)
		return by_id;
	return by_pointer ? by_pointer : by_id;
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

static void detect_callback(const RockIvaDetectResult *result,
				    const RockIvaExecuteStatus status, void *userdata)
{
	struct probe_state *state = userdata;
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
		pthread_mutex_unlock(&state->metrics_lock);
		return;
	}
	if (result->channelId != state->channel_id)
		state->channel_mismatches++;
	if (result->frameId >= state->frame_count)
		state->detection_frame_errors++;
	submitted = result->frameId < state->frame_count
			? state->frames[result->frameId].submitted_ns
			: 0;
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
	pthread_mutex_unlock(&state->metrics_lock);
}

static void release_callback(const RockIvaReleaseFrames *frames, void *userdata)
{
	struct probe_state *state = userdata;
	uint32_t i;

	if (!state) {
		fprintf(stderr, "release callback received NULL userdata\n");
		return;
	}
	pthread_mutex_lock(&state->metrics_lock);
	state->release_callbacks++;
	if (!frames) {
		state->release_invalid_callbacks++;
		pthread_mutex_unlock(&state->metrics_lock);
		return;
	}
	state->release_entries += frames->count;
	printf("release channel_id=%" PRIu32 " count=%" PRIu32 "\n",
	       frames->channelId, frames->count);
	if (frames->channelId != state->channel_id)
		state->channel_mismatches++;
	if (frames->count > ROCKIVA_MAX_OBJ_NUM) {
		state->release_invalid_callbacks++;
		state->release_unmatched += frames->count - ROCKIVA_MAX_OBJ_NUM;
	}
	for (i = 0; i < frames->count && i < ROCKIVA_MAX_OBJ_NUM; i++) {
		const RockIvaImage *released = &frames->frames[i];
		struct probe_frame *record = find_frame_locked(state, released);
		uint64_t now = monotonic_ns();
		uint64_t submitted = record ? record->submitted_ns : 0;
		uint8_t *owned_buffer = NULL;

		printf("  released frame_id=%" PRIu32 " fd=%" PRId32 " data=%p phy=%p\n",
		       released->frameId, released->dataFd, (void *)released->dataAddr,
		       (void *)released->dataPhyAddr);
		if (released->channelId != state->channel_id)
			state->channel_mismatches++;
		if (!record || record->state == PROBE_FRAME_UNUSED ||
		    record->state == PROBE_FRAME_REJECTED) {
			state->release_unmatched++;
			continue;
		}
		if (record->state == PROBE_FRAME_RELEASED) {
			state->release_duplicates++;
			continue;
		}
		if (record->buffer && released->dataAddr != record->buffer) {
			state->release_mismatches++;
			state->release_unmatched++;
			fprintf(stderr, "release frame_id=%" PRIu32
				" does not return the submitted CPU buffer\n",
				released->frameId);
			continue;
		}
		update_latency(submitted, now, &state->release_latency_count,
			       &state->release_latency_sum_ns,
			       &state->release_latency_min_ns,
			       &state->release_latency_max_ns, "release");
		owned_buffer = record->buffer;
		record->buffer = NULL;
		record->state = PROBE_FRAME_RELEASED;
		state->released_frames++;
		/* Only free buffers allocated by this probe, never arbitrary SDK pointers. */
		free(owned_buffer);
	}
	pthread_mutex_unlock(&state->metrics_lock);
}

static int read_frame(FILE *input, uint8_t *buffer, size_t frame_size)
{
	return fread(buffer, 1, frame_size, input) == frame_size ? 0 : -1;
}

static void sleep_for_frame(unsigned int fps)
{
	struct timespec delay;

	delay.tv_sec = 0;
	delay.tv_nsec = (long)(UINT64_C(1000000000) / fps);
	nanosleep(&delay, NULL);
}

static uint8_t *mark_push_failure_locked(struct probe_state *state, uint32_t frame_id)
{
	struct probe_frame *record;
	uint8_t *buffer = NULL;

	if (frame_id >= state->frame_count)
		return NULL;
	record = &state->frames[frame_id];
	if (record->state == PROBE_FRAME_PENDING) {
		buffer = record->buffer;
		record->buffer = NULL;
		record->state = PROBE_FRAME_REJECTED;
	}
	return buffer;
}

static void free_unreturned_buffers(struct probe_state *state)
{
	size_t i;

	pthread_mutex_lock(&state->metrics_lock);
	for (i = 0; i < state->frame_count; i++) {
		if (state->frames[i].buffer) {
			free(state->frames[i].buffer);
			state->frames[i].buffer = NULL;
			if (state->frames[i].state == PROBE_FRAME_PENDING ||
			    state->frames[i].state == PROBE_FRAME_ACCEPTED)
				state->frames[i].state = PROBE_FRAME_REJECTED;
		}
	}
	pthread_mutex_unlock(&state->metrics_lock);
}

static int wait_finish(RockIvaHandle handle, int timeout_ms, const char *label)
{
	RockIvaRetCode ret = ROCKIVA_WaitFinish(handle, -1, timeout_ms);

	printf("%s ret=%d\n", label, ret);
	return ret == ROCKIVA_RET_SUCCESS ? 0 : -1;
}

int main(int argc, char **argv)
{
	struct probe_options options;
	static struct probe_state state;
	RockIvaInitParam init_params;
	RockIvaDetTaskParams det_params;
	RockIvaHandle handle = NULL;
	RockIvaRetCode ret;
	FILE *input = NULL;
	uint64_t frame_size;
	unsigned int frame;
	char version[MAX_VERSION] = {0};
	int parse_result;
	int operation_failed = 0;
	int metrics_lock_initialized = 0;
	int handle_initialized = 0;
	int detect_initialized = 0;
	int wait_succeeded = 0;
	int final_wait_succeeded = 0;
	int release_succeeded = 0;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0)
		return parse_result > 0 ? 0 : 2;

	memset(&state, 0, sizeof(state));
	if (pthread_mutex_init(&state.metrics_lock, NULL) != 0) {
		fprintf(stderr, "cannot initialize metrics lock\n");
		return 2;
	}
	metrics_lock_initialized = 1;
	state.channel_id = options.channel_id;
	if (options.width > UINT16_MAX || options.height > UINT16_MAX) {
		fprintf(stderr, "frame dimensions exceed RockIVA limits\n");
		operation_failed = 1;
		goto done;
	}
	frame_size = (uint64_t)options.width * options.height * 3U / 2U;
	if (frame_size > SIZE_MAX) {
		fprintf(stderr, "frame size is too large\n");
		operation_failed = 1;
		goto done;
	}
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
	input = fopen(options.input_path, "rb");
	if (!input) {
		fprintf(stderr, "cannot open input %s: %s\n", options.input_path, strerror(errno));
		operation_failed = 1;
		goto done;
	}

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
	init_params.imageInfo.width = (uint16_t)options.width;
	init_params.imageInfo.height = (uint16_t)options.height;
	init_params.imageInfo.wstride = (uint16_t)options.width;
	init_params.imageInfo.hstride = (uint16_t)options.height;
	init_params.imageInfo.format = ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12;
	init_params.imageInfo.transformMode = ROCKIVA_IMAGE_TRANSFORM_NONE;

	memset(&det_params, 0, sizeof(det_params));
	det_params.detObjectType = ROCKIVA_OBJECT_TYPE_BITMASK(ROCKIVA_OBJECT_TYPE_PERSON);

	printf("probe model=%s model_path=%s input=%s size=%ux%u frames=%u fps=%u "
	       "channel=%u core_mask=0x%x min_person=%u min_tracking=%u\n",
	       model_name(options.model), options.model_path, options.input_path, options.width,
	       options.height, options.frames, options.fps, options.channel_id,
	       options.core_mask, options.min_person_observations,
	       options.min_tracking_observations);
	ret = ROCKIVA_GetVersion(MAX_VERSION, version);
	printf("rockiva version ret=%d value=%s\n", ret, version[0] ? version : "(unavailable)");
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
		goto release_handle;
	}
	detect_initialized = 1;
	ret = ROCKIVA_SetFrameReleaseCallback(handle, release_callback);
	if (ret != ROCKIVA_RET_SUCCESS) {
		fprintf(stderr, "ROCKIVA_SetFrameReleaseCallback failed: %d\n", ret);
		operation_failed = 1;
		goto release_detect;
	}

	for (frame = 0; frame < options.frames; frame++) {
		RockIvaImage image;
		uint8_t *frame_buffer;
		uint64_t submitted;
		struct probe_frame *record;
		uint8_t *failed_buffer;

		frame_buffer = malloc((size_t)frame_size);
		if (!frame_buffer) {
			fprintf(stderr, "cannot allocate %" PRIu64 " bytes for frame %u\n",
				frame_size, frame + 1);
			operation_failed = 1;
			goto release_detect;
		}
		if (read_frame(input, frame_buffer, (size_t)frame_size) != 0) {
			fprintf(stderr, "input ended before frame %u\n", frame + 1);
			free(frame_buffer);
			operation_failed = 1;
			goto release_detect;
		}
		memset(&image, 0, sizeof(image));
		image.frameId = frame + 1;
		image.channelId = options.channel_id;
		image.info = init_params.imageInfo;
		image.size = 0;
		image.dataAddr = frame_buffer;
		image.dataFd = -1;
		submitted = monotonic_ns();
		pthread_mutex_lock(&state.metrics_lock);
		record = &state.frames[image.frameId];
		record->buffer = frame_buffer;
		record->submitted_ns = submitted;
		record->state = PROBE_FRAME_PENDING;
		pthread_mutex_unlock(&state.metrics_lock);
		ret = ROCKIVA_PushFrame(handle, &image, NULL);
		failed_buffer = NULL;
		pthread_mutex_lock(&state.metrics_lock);
		if (ret != ROCKIVA_RET_SUCCESS) {
			state.push_failures++;
			failed_buffer = mark_push_failure_locked(&state, image.frameId);
		} else {
			state.pushed++;
			if (record->state == PROBE_FRAME_PENDING)
				record->state = PROBE_FRAME_ACCEPTED;
		}
		pthread_mutex_unlock(&state.metrics_lock);
		if (ret != ROCKIVA_RET_SUCCESS) {
			free(failed_buffer);
			fprintf(stderr, "push frame %u failed: %d\n", frame + 1, ret);
			operation_failed = 1;
			continue;
		}
		printf("push frame_id=%u ret=%d\n", frame + 1, ret);
		sleep_for_frame(options.fps);
	}
	if (wait_finish(handle, options.timeout_ms, "wait_finish") == 0)
		wait_succeeded = 1;
	else
		operation_failed = 1;

release_detect:
	if (!wait_succeeded && wait_finish(handle, options.timeout_ms,
						  "cleanup_wait_finish") == 0)
		wait_succeeded = 1;
	if (detect_initialized) {
		if (!wait_succeeded) {
			fprintf(stderr, "skipping ROCKIVA_DETECT_Release after wait failure\n");
		} else {
			ret = ROCKIVA_DETECT_Release(handle);
			printf("detect_release ret=%d\n", ret);
			if (ret != ROCKIVA_RET_SUCCESS)
				operation_failed = 1;
		}
	}
release_handle:
	if (handle_initialized) {
		if (wait_finish(handle, options.timeout_ms, "final_wait_finish") == 0)
			final_wait_succeeded = 1;
		else
			operation_failed = 1;
		/* Always tear down the SDK; an unsuccessful wait only disables buffer cleanup. */
		ret = ROCKIVA_Release(handle);
		printf("release ret=%d\n", ret);
		if (ret != ROCKIVA_RET_SUCCESS)
			operation_failed = 1;
		else
			release_succeeded = 1;
		if (release_succeeded)
			handle = NULL;
	}
done:
	if (handle_initialized && (!final_wait_succeeded || !release_succeeded))
		fprintf(stderr, "frame ownership cleanup deferred after incomplete SDK shutdown\n");
	if (handle_initialized && final_wait_succeeded && release_succeeded)
		free_unreturned_buffers(&state);
	else if (handle_initialized)
		fprintf(stderr, "unreturned buffers remain owned by RockIVA after shutdown failure\n");
	pthread_mutex_lock(&state.metrics_lock);
	if (state.pushed != options.frames || state.push_failures != 0 ||
	    state.released_frames != state.pushed || state.detection_errors != 0 ||
	    state.detection_frame_errors != 0 || state.detection_object_overflows != 0 ||
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
	if (handle_initialized && (!final_wait_succeeded || !release_succeeded))
		operation_failed = 1;
	if (handle_initialized && release_succeeded &&
	    state.released_frames != state.pushed)
		fprintf(stderr, "SDK shutdown completed with unreturned frames\n");
	printf("summary pushed=%" PRIu64 " push_failures=%" PRIu64
	       " detection_callbacks=%" PRIu64 " detection_errors=%" PRIu64
	       " person=%" PRIu64 " person_states[first=%" PRIu64
	       " tracking=%" PRIu64 " lost=%" PRIu64 " disappear=%" PRIu64 "]"
	       " release_callbacks=%" PRIu64 " release_entries=%" PRIu64
	       " released_frames=%" PRIu64 " release_unmatched=%" PRIu64
	       " release_duplicates=%" PRIu64 " release_mismatches=%" PRIu64
	       " release_invalid=%" PRIu64 " channel_mismatches=%" PRIu64
	       " detect_latency_ms[min=%.3f max=%.3f avg=%.3f]"
	       " release_latency_ms[min=%.3f max=%.3f avg=%.3f]\n",
	       state.pushed, state.push_failures, state.detections, state.detection_errors,
	       state.person_observations, state.person_first_observations,
	       state.person_tracking_observations, state.person_lost_observations,
	       state.person_disappear_observations,
	       state.release_callbacks, state.release_entries, state.released_frames,
	       state.release_unmatched, state.release_duplicates, state.release_mismatches,
	       state.release_invalid_callbacks, state.channel_mismatches,
	       (double)state.detection_latency_min_ns / 1000000.0,
	       (double)state.detection_latency_max_ns / 1000000.0,
	       state.detection_latency_count ? (double)state.detection_latency_sum_ns /
	                                              (double)state.detection_latency_count / 1000000.0
	                                      : 0.0,
	       (double)state.release_latency_min_ns / 1000000.0,
	       (double)state.release_latency_max_ns / 1000000.0,
	       state.release_latency_count ? (double)state.release_latency_sum_ns /
                                             (double)state.release_latency_count / 1000000.0
                                     : 0.0);
	pthread_mutex_unlock(&state.metrics_lock);
	/* Keep the table alive when the SDK did not confirm all asynchronous work ended. */
	if (!handle_initialized || (final_wait_succeeded && release_succeeded)) {
		free(state.frames);
		state.frames = NULL;
	}
	if (input && fclose(input) != 0) {
		fprintf(stderr, "cannot close input %s: %s\n", options.input_path,
			strerror(errno));
		operation_failed = 1;
	}
	if (metrics_lock_initialized &&
	    (!handle_initialized || (final_wait_succeeded && release_succeeded)))
		pthread_mutex_destroy(&state.metrics_lock);
	return operation_failed ? 1 : 0;
}
