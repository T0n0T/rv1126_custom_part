/* RV1126B INT8 YOLO26/YOLOv8: RGB replay or live camera with screen preview. */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "rknn_api.h"
#include "flow_camera.h"
#include "flow_events.h"
#include "flow_yolov8.h"

#define INPUT_WIDTH 640U
#define INPUT_HEIGHT 640U
#define INPUT_CHANNELS 3U
#define INPUT_BYTES ((size_t)INPUT_WIDTH * INPUT_HEIGHT * INPUT_CHANNELS)
#define ANCHORS 8400U
#define TOPK 300U
#define MAX_METADATA_LINE 512U

struct options {
	const char *model_path;
	const char *frames_path;
	const char *metadata_path;
	const char *detections_path;
	double confidence;
	uint64_t max_frames;
	struct flow_camera_options camera;
	const char *zones_path;
	const char *session_id;
	const char *event_directory;
	int debug_zones;
};

static volatile sig_atomic_t stop_requested;

static void request_stop(int signal_number)
{
	(void)signal_number;
	stop_requested = 1;
}

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s --model MODEL.rknn --detections detections.jsonl\n"
		"  file: --frames frames.rgb --metadata frames.tsv\n"
		"  live: --camera /dev/video25 [--camera-id 0] [--iq-dir /oem/usr/share/iqfiles]\n"
		"        [--width 640 --height 360 --fps 30 --rotation 180 --camera-color-order BGR]\n"
		"        [--connector-id 97 --plane-id 75 --screen-width 480 --screen-height 800]\n"
		"        [--screen-rotation 270] (clockwise; 270 = counterclockwise 90)\n"
		"        [--stats stats.json] [--snapshot preview.ppm] [--no-display]\n"
		"        [--source-snapshot source.ppm] (clean rotated image for zone calibration)\n"
		"        [--zones camera-zones.json --session-id NAME --event-dir DIR] [--debug-zones]\n"
		"  common: [--conf 0.25] [--max-frames N] (0 = unlimited)\n"
		"  model: auto-detect YOLO26 split INT8 or single-class YOLOv8 DFL six-head INT8\n", program);
}

static int parse_integer(const char *value, int minimum, int maximum, int *result)
{
	char *end;
	long number;
	errno = 0;
	number = strtol(value, &end, 10);
	if (errno || end == value || *end || number < minimum || number > maximum)
		return -1;
	*result = (int)number;
	return 0;
}

struct candidate {
	float score;
	uint32_t anchor;
	uint32_t class_id;
	uint32_t order;
};

static uint64_t monotonic_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
		return 0;
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

static int read_file(const char *path, uint8_t **data, size_t *size)
{
	FILE *file = fopen(path, "rb");
	long length;
	uint8_t *buffer;
	size_t count;
	if (!file) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
	    fseek(file, 0, SEEK_SET) != 0 || (uint64_t)length > UINT32_MAX) {
		fprintf(stderr, "invalid model size: %s\n", path);
		fclose(file);
		return -1;
	}
	buffer = malloc((size_t)length);
	if (!buffer) {
		fclose(file);
		return -1;
	}
	count = fread(buffer, 1, (size_t)length, file);
	fclose(file);
	if (count != (size_t)length) {
		fprintf(stderr, "short read for model: %s\n", path);
		free(buffer);
		return -1;
	}
	*data = buffer;
	*size = count;
	return 0;
}

static int parse_options(int argc, char **argv, struct options *options)
{
	enum { OPT_CAMERA = 1000, OPT_CAMERA_ID, OPT_IQ, OPT_WIDTH, OPT_HEIGHT, OPT_FPS,
	       OPT_ROTATION, OPT_CONNECTOR, OPT_PLANE, OPT_SCREEN_WIDTH, OPT_SCREEN_HEIGHT,
	       OPT_STATS, OPT_SNAPSHOT, OPT_NO_DISPLAY, OPT_ZONES, OPT_SESSION, OPT_EVENTS, OPT_DEBUG, OPT_SOURCE_SNAPSHOT,
	       OPT_SCREEN_ROTATION, OPT_CAMERA_COLOR_ORDER };
	static const struct option long_options[] = {
		{"model", required_argument, NULL, 'm'},
		{"frames", required_argument, NULL, 'f'},
		{"metadata", required_argument, NULL, 't'},
		{"detections", required_argument, NULL, 'd'},
		{"conf", required_argument, NULL, 'c'},
		{"max-frames", required_argument, NULL, 'n'},
		{"camera", required_argument, NULL, OPT_CAMERA},
		{"camera-id", required_argument, NULL, OPT_CAMERA_ID},
		{"iq-dir", required_argument, NULL, OPT_IQ},
		{"width", required_argument, NULL, OPT_WIDTH},
		{"height", required_argument, NULL, OPT_HEIGHT},
		{"fps", required_argument, NULL, OPT_FPS},
		{"rotation", required_argument, NULL, OPT_ROTATION},
		{"screen-rotation", required_argument, NULL, OPT_SCREEN_ROTATION},
		{"camera-color-order", required_argument, NULL, OPT_CAMERA_COLOR_ORDER},
		{"connector-id", required_argument, NULL, OPT_CONNECTOR},
		{"plane-id", required_argument, NULL, OPT_PLANE},
		{"screen-width", required_argument, NULL, OPT_SCREEN_WIDTH},
		{"screen-height", required_argument, NULL, OPT_SCREEN_HEIGHT},
		{"stats", required_argument, NULL, OPT_STATS},
		{"snapshot", required_argument, NULL, OPT_SNAPSHOT},
		{"source-snapshot", required_argument, NULL, OPT_SOURCE_SNAPSHOT},
		{"no-display", no_argument, NULL, OPT_NO_DISPLAY},
		{"zones", required_argument, NULL, OPT_ZONES},
		{"session-id", required_argument, NULL, OPT_SESSION},
		{"event-dir", required_argument, NULL, OPT_EVENTS},
		{"debug-zones", no_argument, NULL, OPT_DEBUG},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int option;

	options->confidence = 0.25;
	options->camera = (struct flow_camera_options){
		.iq_dir = "/oem/usr/share/iqfiles", .camera_id = 0,
		.width = 640, .height = 360, .fps = 30, .rotation = 180, .screen_rotation = 270, .input_bgr = 1,
		.connector_id = 97, .plane_id = 75, .screen_width = 480, .screen_height = 800};
	while ((option = getopt_long(argc, argv, "m:f:t:d:c:n:h", long_options, NULL)) != -1) {
		char *end = NULL;
		errno = 0;
		switch (option) {
		case 'm': options->model_path = optarg; break;
		case 'f': options->frames_path = optarg; break;
		case 't': options->metadata_path = optarg; break;
		case 'd': options->detections_path = optarg; break;
		case 'c':
			options->confidence = strtod(optarg, &end);
			if (errno || end == optarg || *end || !isfinite(options->confidence) ||
			    options->confidence < 0 || options->confidence > 1)
				return -1;
			break;
		case 'n':
			if (optarg[0] == '-')
				return -1;
			options->max_frames = strtoull(optarg, &end, 10);
			if (errno || end == optarg || *end)
				return -1;
			break;
		case OPT_CAMERA: options->camera.device = optarg; break;
		case OPT_IQ: options->camera.iq_dir = optarg; break;
		case OPT_STATS: options->camera.stats_path = optarg; break;
		case OPT_SNAPSHOT: options->camera.snapshot_path = optarg; break;
		case OPT_SOURCE_SNAPSHOT: options->camera.source_snapshot_path = optarg; break;
		case OPT_NO_DISPLAY: options->camera.no_display = 1; break;
		case OPT_ZONES: options->zones_path = optarg; break;
		case OPT_SESSION: options->session_id = optarg; break;
		case OPT_EVENTS: options->event_directory = optarg; break;
		case OPT_DEBUG: options->debug_zones = 1; break;
		case OPT_CAMERA_COLOR_ORDER:
			if (strcmp(optarg, "BGR") == 0) options->camera.input_bgr = 1;
			else if (strcmp(optarg, "RGB") == 0) options->camera.input_bgr = 0;
			else return -1;
			break;
		case OPT_CAMERA_ID:
			if (parse_integer(optarg, 0, 7, &options->camera.camera_id)) return -1;
			break;
		case OPT_WIDTH:
			if (parse_integer(optarg, 32, 1920, &options->camera.width)) return -1;
			break;
		case OPT_HEIGHT:
			if (parse_integer(optarg, 32, 2160, &options->camera.height)) return -1;
			break;
		case OPT_FPS:
			if (parse_integer(optarg, 1, 60, &options->camera.fps)) return -1;
			break;
		case OPT_ROTATION:
			if (parse_integer(optarg, 0, 270, &options->camera.rotation) || options->camera.rotation % 90)
				return -1;
			break;
		case OPT_SCREEN_ROTATION:
			if (parse_integer(optarg, 0, 270, &options->camera.screen_rotation) || options->camera.screen_rotation % 90)
				return -1;
			break;
		case OPT_CONNECTOR:
			if (parse_integer(optarg, 0, INT32_MAX, &options->camera.connector_id)) return -1;
			break;
		case OPT_PLANE:
			if (parse_integer(optarg, 0, INT32_MAX, &options->camera.plane_id)) return -1;
			break;
		case OPT_SCREEN_WIDTH:
			if (parse_integer(optarg, 64, 1920, &options->camera.screen_width)) return -1;
			break;
		case OPT_SCREEN_HEIGHT:
			if (parse_integer(optarg, 64, 1920, &options->camera.screen_height)) return -1;
			break;
		case 'h': usage(argv[0]); exit(0);
		default: return -1;
		}
	}
	if (optind != argc || !options->model_path || !options->detections_path)
		return -1;
	if (options->zones_path && (!options->camera.device || !options->session_id || !options->event_directory))
		return -1;
	if (!options->zones_path && (options->session_id || options->event_directory || options->debug_zones))
		return -1;
	if (options->camera.device)
		return options->frames_path || options->metadata_path ? -1 : 0;
	return options->frames_path && options->metadata_path && !options->camera.stats_path &&
	       !options->camera.snapshot_path && !options->camera.source_snapshot_path ? 0 : -1;
}

static int query_attr(rknn_context context, rknn_query_cmd command,
		      uint32_t index, rknn_tensor_attr *attr)
{
	memset(attr, 0, sizeof(*attr));
	attr->index = index;
	return rknn_query(context, command, attr, sizeof(*attr));
}

static int frame_metadata_read(FILE *file, struct frame_metadata *metadata)
{
	char line[MAX_METADATA_LINE];
	char extra;
	long long frame_index;
	unsigned int width, height;
	int fields;

	if (!fgets(line, sizeof(line), file))
		return feof(file) ? 0 : -1;
	fields = sscanf(line, "%lld %lf %u %u %lf %d %d %c", &frame_index,
			&metadata->pts_ms, &width, &height, &metadata->resize_scale,
			&metadata->pad_left, &metadata->pad_top, &extra);
	if (fields != 7 || frame_index < 0 || !isfinite(metadata->pts_ms) ||
	    !isfinite(metadata->resize_scale) || metadata->resize_scale <= 0 ||
	    width == 0 || height == 0 || metadata->pad_left < 0 || metadata->pad_top < 0 ||
	    metadata->pad_left >= (int32_t)INPUT_WIDTH || metadata->pad_top >= (int32_t)INPUT_HEIGHT)
		return -1;
	metadata->frame_index = (int64_t)frame_index;
	metadata->source_width = width;
	metadata->source_height = height;
	return 1;
}

static double clamp_unit(double value);

static int candidate_descending(const void *left, const void *right)
{
	const struct candidate *a = left;
	const struct candidate *b = right;
	if (a->score != b->score)
		return a->score > b->score ? -1 : 1;
	return (a->order > b->order) - (a->order < b->order);
}

static void dequantize_detections(const int8_t *scores, const int8_t *boxes,
				  const rknn_tensor_attr *score_attr,
				  const rknn_tensor_attr *box_attr,
				  const struct frame_metadata *metadata,
				  double confidence, struct candidate *anchors,
				  struct candidate *candidates,
				  FILE *output, uint64_t *class_counts,
				  struct flow_detection *detections, size_t *detection_count)
{
	uint32_t i;
	uint32_t selected_count = 0;
	*detection_count = 0;
	qsort(anchors, ANCHORS, sizeof(*anchors), candidate_descending);
	for (i = 0; i < TOPK; ++i) {
		const uint32_t anchor = anchors[i].anchor;
		uint32_t class_id;
		for (class_id = 0; class_id < 2; ++class_id) {
			const int8_t q = scores[class_id * ANCHORS + anchor];
			candidates[2 * i + class_id] = (struct candidate){
				((float)q - (float)score_attr->zp) * score_attr->scale,
				anchor, class_id, 2 * i + class_id};
		}
	}
	qsort(candidates, 2 * TOPK, sizeof(*candidates), candidate_descending);
	if (fprintf(output, "{\"frame_index\":%" PRId64 ",\"pts_ms\":%.6f,\"detections\":[",
		    metadata->frame_index, metadata->pts_ms) < 0)
		return;
	for (i = 0; i < TOPK; ++i) {
		const struct candidate *candidate = &candidates[i];
		float coords[4];
		double x1, y1, x2, y2;
		uint32_t corner;
		if (candidate->score < confidence)
			continue;
		for (corner = 0; corner < 4; ++corner) {
			const int8_t q = boxes[corner * ANCHORS + candidate->anchor];
			coords[corner] = ((float)q - (float)box_attr->zp) * box_attr->scale;
		}
		x1 = clamp_unit(((double)coords[0] - metadata->pad_left) /
			metadata->resize_scale / metadata->source_width);
		y1 = clamp_unit(((double)coords[1] - metadata->pad_top) /
			metadata->resize_scale / metadata->source_height);
		x2 = clamp_unit(((double)coords[2] - metadata->pad_left) /
			metadata->resize_scale / metadata->source_width);
		y2 = clamp_unit(((double)coords[3] - metadata->pad_top) /
			metadata->resize_scale / metadata->source_height);
		if (x2 <= x1 || y2 <= y1)
			continue;
		if (fprintf(output, "%s{\"xyxy\":[%.9g,%.9g,%.9g,%.9g],\"score\":%.9g,\"class_id\":%u}",
			    selected_count ? "," : "", x1, y1, x2, y2,
			    candidate->score, candidate->class_id) < 0)
			return;
		++selected_count;
		++class_counts[candidate->class_id];
		detections[(*detection_count)++] = (struct flow_detection){
			{x1, y1, x2, y2}, candidate->score, candidate->class_id};
	}
	(void)fprintf(output, "]}\n");
}

static int valid_dims(const rknn_tensor_attr *attr, uint32_t a, uint32_t b,
		      uint32_t c, uint32_t d)
{
	return attr->n_dims == 4 && attr->dims[0] == a && attr->dims[1] == b &&
	       attr->dims[2] == c && attr->dims[3] == d;
}

static int valid_output(const rknn_tensor_attr *attr, uint32_t channels)
{
	return attr->n_dims == 3 && attr->dims[0] == 1 && attr->dims[1] == channels &&
	       attr->dims[2] == ANCHORS && attr->type == RKNN_TENSOR_INT8 &&
	       attr->fmt == RKNN_TENSOR_UNDEFINED &&
	       attr->qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
	       isfinite(attr->scale) && attr->scale > 0 && attr->zp >= -128 && attr->zp <= 127;
}

static double clamp_unit(double value)
{
	if (value < 0)
		return 0;
	if (value > 1)
		return 1;
	return value;
}

static void write_detections(FILE *output, const struct frame_metadata *metadata,
			     const struct flow_detection *detections, size_t count,
			     uint64_t class_counts[2])
{
	fprintf(output, "{\"frame_index\":%" PRId64 ",\"pts_ms\":%.6f,\"detections\":[",
		metadata->frame_index, metadata->pts_ms);
	for (size_t i = 0; i < count; ++i) {
		const struct flow_detection *d = detections + i;
		fprintf(output, "%s{\"xyxy\":[%.9g,%.9g,%.9g,%.9g],\"score\":%.9g,\"class_id\":%u}",
			i ? "," : "", d->xyxy[0], d->xyxy[1], d->xyxy[2], d->xyxy[3], d->score, d->class_id);
		++class_counts[d->class_id];
	}
	fprintf(output, "]}\n");
}

int main(int argc, char **argv)
{
	struct options options = {0};
	struct candidate *anchors = NULL, *candidates = NULL;
	struct frame_metadata metadata, previous = {0};
	struct flow_detection detections[FLOW_MAX_DETECTIONS];
	struct flow_camera *camera = NULL;
	struct flow_events *events = NULL;
	struct flow_yolov8 *yolov8 = NULL;
	struct flow_overlay overlay = {0};
	size_t detection_count = 0;
	uint8_t *model = NULL, *frame = NULL;
	size_t model_size = 0;
	FILE *frames = NULL, *metadata_file = NULL, *output = NULL;
	rknn_context context = 0;
	rknn_input_output_num io_num = {0};
	rknn_tensor_attr input_attr;
	rknn_tensor_attr output_attrs[FLOW_YOLOV8_OUTPUTS];
	rknn_input input = {0};
	rknn_output outputs[FLOW_YOLOV8_OUTPUTS] = {{0}};
	rknn_sdk_version sdk_version;
	uint64_t class_counts[2] = {0, 0};
	uint64_t frame_count = 0, total_infer_ns = 0, total_post_ns = 0;
	int status = 1;
	int metadata_status;

	if (parse_options(argc, argv, &options) != 0) {
		usage(argv[0]);
		return 2;
	}
	if (options.camera.device) {
		struct sigaction action = {0};
		action.sa_handler = request_stop;
		sigemptyset(&action.sa_mask);
		if (sigaction(SIGINT, &action, NULL) || sigaction(SIGTERM, &action, NULL))
			return 1;
	}
	if (read_file(options.model_path, &model, &model_size) != 0)
		goto cleanup;
	if (!options.camera.device) {
		frames = fopen(options.frames_path, "rb");
		metadata_file = fopen(options.metadata_path, "r");
	}
	output = fopen(options.detections_path, options.camera.device ? "wx" : "w");
	if ((!options.camera.device && (!frames || !metadata_file)) || !output) {
		fprintf(stderr, "cannot open frame stream, metadata, or detections output: %s\n",
			strerror(errno));
		goto cleanup;
	}
	frame = malloc(INPUT_BYTES);
	anchors = malloc(ANCHORS * sizeof(*anchors));
	candidates = malloc(2 * TOPK * sizeof(*candidates));
	if (!frame || !anchors || !candidates)
		goto cleanup;
	if (rknn_init(&context, model, (uint32_t)model_size, 0, NULL) < 0) {
		fprintf(stderr, "rknn_init failed\n");
		goto cleanup;
	}
	memset(&sdk_version, 0, sizeof(sdk_version));
	if (rknn_query(context, RKNN_QUERY_SDK_VERSION, &sdk_version, sizeof(sdk_version)) == 0)
		(void)fprintf(stderr, "sdk_api=%s driver=%s\n", sdk_version.api_version,
			       sdk_version.drv_version);
	else
		(void)fprintf(stderr, "sdk version query failed\n");
	if (rknn_query(context, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != 0 ||
	    io_num.n_input != 1 || (io_num.n_output != 2 && io_num.n_output != FLOW_YOLOV8_OUTPUTS) ||
	    query_attr(context, RKNN_QUERY_INPUT_ATTR, 0, &input_attr) != 0 ||
	    !valid_dims(&input_attr, 1, INPUT_HEIGHT, INPUT_WIDTH, INPUT_CHANNELS) ||
	    input_attr.fmt != RKNN_TENSOR_NHWC || input_attr.type != RKNN_TENSOR_INT8 ||
	    input_attr.qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
	    input_attr.zp != -128 || fabs(input_attr.scale - (1.0 / 255.0)) > 1e-6 ||
	    input_attr.n_elems != INPUT_BYTES) {
		fprintf(stderr, "model must have 640x640 RGB INT8 input and supported INT8 output heads\n");
		goto cleanup;
	}
	for (uint32_t i = 0; i < io_num.n_output; ++i) {
		if (query_attr(context, RKNN_QUERY_OUTPUT_ATTR, i, &output_attrs[i]) != 0)
			goto cleanup;
		outputs[i].index = i;
		outputs[i].want_float = 0;
	}
	if (io_num.n_output == FLOW_YOLOV8_OUTPUTS) {
		yolov8 = flow_yolov8_create(output_attrs, io_num.n_output);
		if (!yolov8) {
			fprintf(stderr, "model tensor contract does not match single-class YOLOv8 DFL INT8 heads\n");
			goto cleanup;
		}
	} else if (!valid_output(&output_attrs[0], 2) || !valid_output(&output_attrs[1], 4) ||
	    strcmp(output_attrs[0].name, "/model.23/Sigmoid_output_0") != 0 ||
	    strcmp(output_attrs[1].name, "/model.23/Mul_2_output_0") != 0 ||
	    output_attrs[0].size != 2 * ANCHORS || output_attrs[1].size != 4 * ANCHORS) {
		fprintf(stderr, "model tensor contract does not match YOLO26 split INT8 outputs\n");
		goto cleanup;
	}
	(void)fprintf(stderr, "detector=%s input=%s bytes=%u outputs=%u\n",
		       yolov8 ? "yolov8-helmet-dfl-int8" : "yolo26-split-int8", input_attr.name, input_attr.size, io_num.n_output);
	for (uint32_t i = 0; i < io_num.n_output; ++i)
		fprintf(stderr, "output%u=%s zp=%" PRId32 " scale=%.9g bytes=%u\n",
			i, output_attrs[i].name, output_attrs[i].zp, output_attrs[i].scale, output_attrs[i].size);
	input.index = 0;
	input.buf = frame;
	input.size = (uint32_t)INPUT_BYTES;
	input.pass_through = 0;
	input.type = RKNN_TENSOR_UINT8;
	input.fmt = RKNN_TENSOR_NHWC;
	if (options.zones_path) {
		int sw = options.camera.rotation % 180 ? options.camera.height : options.camera.width;
		int sh = options.camera.rotation % 180 ? options.camera.width : options.camera.height;
		events = flow_events_open(options.zones_path, options.session_id, options.event_directory,
			options.camera.rotation, sw, sh, options.debug_zones);
		if (!events) goto cleanup;
	}
	if (options.camera.device && !(camera = flow_camera_open(&options.camera)))
		goto cleanup;

	while (!stop_requested && (options.max_frames == 0 || frame_count < options.max_frames)) {
		uint64_t start, infer_end, post_end;
		int runtime_status;
		metadata_status = camera ? flow_camera_next(camera, frame, &metadata, &stop_requested) :
			frame_metadata_read(metadata_file, &metadata);
		if (metadata_status == 0)
			break;
		if (metadata_status < 0 ||
		    (frame_count > 0 && (metadata.frame_index <= previous.frame_index ||
					 metadata.pts_ms < previous.pts_ms))) {
			fprintf(stderr, "invalid/nonmonotonic frame metadata at row=%" PRIu64 "\n", frame_count);
			goto cleanup;
		}
		if (!camera && fread(frame, 1, INPUT_BYTES, frames) != INPUT_BYTES) {
			fprintf(stderr, "short RGB frame at index=%" PRId64 "\n", metadata.frame_index);
			goto cleanup;
		}
		for (uint32_t i = 0; i < io_num.n_output; ++i) outputs[i].buf = NULL;
		start = monotonic_ns();
		runtime_status = rknn_inputs_set(context, 1, &input);
		if (runtime_status == 0)
			runtime_status = rknn_run(context, NULL);
		if (runtime_status == 0)
			runtime_status = rknn_outputs_get(context, io_num.n_output, outputs, NULL);
		infer_end = monotonic_ns();
		int any_output = 0;
		for (uint32_t i = 0; i < io_num.n_output; ++i) {
			any_output |= outputs[i].buf != NULL;
			if (!outputs[i].buf || outputs[i].size != output_attrs[i].size) runtime_status = -1;
		}
		if (runtime_status != 0) {
			fprintf(stderr, "RKNN inference/output contract failed at index=%" PRId64 "\n",
				metadata.frame_index);
			if (any_output)
				(void)rknn_outputs_release(context, io_num.n_output, outputs);
			goto cleanup;
		}
		if (yolov8) {
			if (flow_yolov8_decode(yolov8, outputs, io_num.n_output, &metadata, options.confidence,
					       0.7, detections, &detection_count) != 0) {
				fprintf(stderr, "YOLOv8 output decode failed\n");
				(void)rknn_outputs_release(context, io_num.n_output, outputs);
				goto cleanup;
			}
			write_detections(output, &metadata, detections, detection_count, class_counts);
		} else {
			for (uint32_t i = 0; i < ANCHORS; ++i) {
				const int8_t a = ((const int8_t *)outputs[0].buf)[i];
				const int8_t b = ((const int8_t *)outputs[0].buf)[ANCHORS + i];
				const int8_t maximum = a > b ? a : b;
				anchors[i] = (struct candidate){
					((float)maximum - (float)output_attrs[0].zp) * output_attrs[0].scale,
					i, 0, i};
			}
			dequantize_detections(outputs[0].buf, outputs[1].buf, &output_attrs[0],
					      &output_attrs[1], &metadata, options.confidence,
					      anchors, candidates, output, class_counts, detections, &detection_count);
		}
		post_end = monotonic_ns();
		if (fflush(output) != 0 || ferror(output)) {
			fprintf(stderr, "writing detections failed\n");
			(void)rknn_outputs_release(context, io_num.n_output, outputs);
			goto cleanup;
		}
		if (rknn_outputs_release(context, io_num.n_output, outputs) != 0) {
			fprintf(stderr, "output release failed\n");
			goto cleanup;
		}
		if (events && flow_events_update(events, &metadata, detections, detection_count, &overlay) != 0)
			goto cleanup;
		if (camera && flow_camera_present(camera, frame, &metadata, detections, detection_count,
						(infer_end - start) / 1000000.0,
						(post_end - infer_end) / 1000000.0, events ? &overlay : NULL) != 0)
			goto cleanup;
		total_infer_ns += infer_end - start;
		total_post_ns += post_end - infer_end;
		previous = metadata;
		++frame_count;
		if (frame_count == 1 || frame_count % 25 == 0)
			fprintf(stderr, "frame=%" PRIu64 " source_index=%" PRId64
				" infer_ms=%.3f post_ms=%.3f\n", frame_count,
				metadata.frame_index, (infer_end - start) / 1000000.0,
				(post_end - infer_end) / 1000000.0);
	}
	if (frame_count == 0 && !stop_requested) {
		fprintf(stderr, "no frame metadata was provided\n");
		goto cleanup;
	}
	if (!camera && options.max_frames == 0) {
		int extra = fgetc(frames);
		if (extra != EOF) {
			fprintf(stderr, "RGB stream has more frame data than metadata rows\n");
			goto cleanup;
		}
		if (ferror(frames)) {
			fprintf(stderr, "error reading RGB stream\n");
			goto cleanup;
		}
	}
	if (fflush(output) != 0 || ferror(output)) {
		fprintf(stderr, "writing detections failed\n");
		goto cleanup;
	}
	(void)fprintf(stderr, "RKNN_YOLO26_INFER_OK frames=%" PRIu64
		       " helmet=%" PRIu64 " head=%" PRIu64 " avg_infer_ms=%.3f avg_post_ms=%.3f\n",
		       frame_count, class_counts[0], class_counts[1],
	       total_infer_ns / (double)(frame_count ? frame_count : 1) / 1000000.0,
	       total_post_ns / (double)(frame_count ? frame_count : 1) / 1000000.0);
	status = 0;

cleanup:
	if (flow_camera_close(camera) != 0)
		status = 1;
	if (flow_events_close(events, status == 0) != 0)
		status = 1;
	if (context)
		rknn_destroy(context);
	flow_yolov8_destroy(yolov8);
	if (output) fclose(output);
	if (metadata_file) fclose(metadata_file);
	if (frames) fclose(frames);
	free(candidates);
	free(anchors);
	free(frame);
	free(model);
	return status;
}
