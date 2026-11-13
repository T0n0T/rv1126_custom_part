/* Portable INT8 YOLOv8n six-head postprocessor for RV1126B output dumps. */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_MAX 16
#define HEAD_COUNT 3
#define MAX_DETECTIONS 300
#define MAX_PATH 4096

struct head_config {
	int stride;
	int size;
	int box_zp;
	float box_scale;
	int score_zp;
	float score_scale;
};

struct detection {
	float x1;
	float y1;
	float x2;
	float y2;
	float score;
	unsigned int order;
};

static const struct head_config heads[HEAD_COUNT] = {
	{8, 80, -14, 0.395262748f, -128, 0.00334408041f},
	{16, 40, -24, 1.18675947f, -128, 0.00356173562f},
	{32, 20, -2, 0.711200655f, -128, 0.00371879875f},
};

static void usage(const char *program)
{
	fprintf(stderr,
		"usage: %s --dump-dir DIR [--conf FLOAT] [--iou FLOAT] [--max-det N]\n",
		program);
}

static int read_tensor(const char *directory, unsigned int index,
		       size_t expected, int8_t **values)
{
	char path[MAX_PATH];
	FILE *file;
	int written;
	int8_t *buffer;
	size_t count;

	written = snprintf(path, sizeof(path), "%s/output-%u.bin", directory, index);
	if (written < 0 || (size_t)written >= sizeof(path)) {
		fprintf(stderr, "output path is too long\n");
		return -1;
	}
	file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	buffer = (int8_t *)malloc(expected);
	if (!buffer) {
		fclose(file);
		fprintf(stderr, "out of memory reading %s\n", path);
		return -1;
	}
	count = fread(buffer, 1, expected, file);
	if (count != expected || fgetc(file) != EOF) {
		fprintf(stderr, "wrong byte count in %s; expected %zu\n", path, expected);
		free(buffer);
		fclose(file);
		return -1;
	}
	fclose(file);
	*values = buffer;
	return 0;
}

static int compare_score_descending(const void *left, const void *right)
{
	const struct detection *a = (const struct detection *)left;
	const struct detection *b = (const struct detection *)right;
	if (a->score > b->score)
		return -1;
	if (a->score < b->score)
		return 1;
	if (a->order < b->order)
		return -1;
	if (a->order > b->order)
		return 1;
	return 0;
}

static float iou(const struct detection *a, const struct detection *b)
{
	float x1 = fmaxf(a->x1, b->x1);
	float y1 = fmaxf(a->y1, b->y1);
	float x2 = fminf(a->x2, b->x2);
	float y2 = fminf(a->y2, b->y2);
	float width = fmaxf(x2 - x1, 0.0f);
	float height = fmaxf(y2 - y1, 0.0f);
	float intersection = width * height;
	float area_a = fmaxf(a->x2 - a->x1, 0.0f) * fmaxf(a->y2 - a->y1, 0.0f);
	float area_b = fmaxf(b->x2 - b->x1, 0.0f) * fmaxf(b->y2 - b->y1, 0.0f);
	float union_area = area_a + area_b - intersection;
	return union_area > 0.0f ? intersection / union_area : 0.0f;
}

static void decode_head(const int8_t *box_q, const int8_t *score_q,
			const struct head_config *head, float confidence,
			struct detection *detections, size_t *detection_count,
			unsigned int *order)
{
	const int area = head->size * head->size;
	float logits[REG_MAX];
	float distances[4];
	int y;
	int x;
	int side;
	int bin;

	for (y = 0; y < head->size; ++y) {
		for (x = 0; x < head->size; ++x) {
			const int cell = y * head->size + x;
			const float score = (float)((int)score_q[cell] - head->score_zp) * head->score_scale;
			float center_x;
			float center_y;
			if (score < confidence)
				continue;
			for (side = 0; side < 4; ++side) {
				float maximum = -INFINITY;
				float denominator = 0.0f;
				float numerator = 0.0f;
				for (bin = 0; bin < REG_MAX; ++bin) {
					const int channel = side * REG_MAX + bin;
					const float value = (float)((int)box_q[channel * area + cell] - head->box_zp) * head->box_scale;
					logits[bin] = value;
					if (value > maximum)
						maximum = value;
				}
				for (bin = 0; bin < REG_MAX; ++bin) {
					const float probability = expf(logits[bin] - maximum);
					denominator += probability;
					numerator += probability * (float)bin;
				}
				distances[side] = denominator > 0.0f
					? numerator / denominator * (float)head->stride
					: 0.0f;
			}
			center_x = ((float)x + 0.5f) * (float)head->stride;
			center_y = ((float)y + 0.5f) * (float)head->stride;
			detections[*detection_count] = (struct detection){
				.x1 = center_x - distances[0],
				.y1 = center_y - distances[1],
				.x2 = center_x + distances[2],
				.y2 = center_y + distances[3],
				.score = fminf(fmaxf(score, 0.0f), 1.0f),
				.order = (*order)++,
			};
			++*detection_count;
		}
	}
}

static size_t apply_nms(struct detection *detections, size_t count,
			float iou_threshold, unsigned int max_det)
{
	size_t selected = 0;
	size_t candidate;
	qsort(detections, count, sizeof(*detections), compare_score_descending);
	for (candidate = 0; candidate < count && selected < max_det; ++candidate) {
		size_t prior;
		int suppressed = 0;
		for (prior = 0; prior < selected; ++prior) {
			if (iou(&detections[candidate], &detections[prior]) > iou_threshold) {
				suppressed = 1;
				break;
			}
		}
		if (!suppressed) {
			if (candidate != selected)
				detections[selected] = detections[candidate];
			++selected;
		}
	}
	return selected;
}

static int parse_float(const char *value, float *result)
{
	char *end = NULL;
	errno = 0;
	*result = strtof(value, &end);
	return errno == 0 && end != value && *end == '\0' && isfinite(*result) ? 0 : -1;
}

static int parse_uint(const char *value, unsigned int *result)
{
	char *end = NULL;
	unsigned long parsed;
	errno = 0;
	parsed = strtoul(value, &end, 10);
	if (errno || end == value || *end || parsed == 0 || parsed > MAX_DETECTIONS)
		return -1;
	*result = (unsigned int)parsed;
	return 0;
}

int main(int argc, char **argv)
{
	const char *dump_dir = NULL;
	float confidence = 0.001f;
	float iou_threshold = 0.7f;
	unsigned int max_det = MAX_DETECTIONS;
	struct detection *detections = NULL;
	size_t detection_count = 0;
	unsigned int order = 0;
	unsigned int i;
	size_t kept;
	int arg;

	for (arg = 1; arg < argc; ++arg) {
		if (!strcmp(argv[arg], "--dump-dir") && arg + 1 < argc)
			dump_dir = argv[++arg];
		else if (!strcmp(argv[arg], "--conf") && arg + 1 < argc) {
			if (parse_float(argv[++arg], &confidence) != 0 || confidence < 0.0f || confidence > 1.0f)
				goto invalid_args;
		} else if (!strcmp(argv[arg], "--iou") && arg + 1 < argc) {
			if (parse_float(argv[++arg], &iou_threshold) != 0 || iou_threshold < 0.0f || iou_threshold > 1.0f)
				goto invalid_args;
		} else if (!strcmp(argv[arg], "--max-det") && arg + 1 < argc) {
			if (parse_uint(argv[++arg], &max_det) != 0)
				goto invalid_args;
		} else {
			goto invalid_args;
		}
	}
	if (!dump_dir)
		goto invalid_args;

	detections = (struct detection *)malloc(8400U * sizeof(*detections));
	if (!detections) {
		fprintf(stderr, "out of memory allocating detection candidates\n");
		return 1;
	}
	for (i = 0; i < HEAD_COUNT; ++i) {
		const size_t area = (size_t)heads[i].size * (size_t)heads[i].size;
		int8_t *box_q = NULL;
		int8_t *score_q = NULL;
		if (read_tensor(dump_dir, 2U * i, 4U * REG_MAX * area, &box_q) != 0 ||
		    read_tensor(dump_dir, 2U * i + 1U, area, &score_q) != 0) {
			free(box_q);
			free(score_q);
			free(detections);
			return 1;
		}
		decode_head(box_q, score_q, &heads[i], confidence, detections,
			    &detection_count, &order);
		free(box_q);
		free(score_q);
	}
	kept = apply_nms(detections, detection_count, iou_threshold, max_det);
	printf("{\"model_input\":{\"width\":640,\"height\":640},\"thresholds\":{\"confidence\":%.9g,\"iou\":%.9g,\"max_det\":%u},\"detections\":[",
	       confidence, iou_threshold, max_det);
	for (i = 0; i < kept; ++i) {
		const struct detection *d = &detections[i];
		printf("%s{\"xyxy\":[%.9g,%.9g,%.9g,%.9g],\"score\":%.9g,\"class_id\":0}",
		       i == 0 ? "" : ",", d->x1, d->y1, d->x2, d->y2, d->score);
	}
	puts("]}");
	free(detections);
	return 0;

invalid_args:
	usage(argv[0]);
	return 2;
}
