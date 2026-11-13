/* YOLO26l split-output INT8 reference: runtime scales, two TopK stages, no NMS. */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ANCHORS 8400
#define TOPK 300

struct candidate {
	float score;
	int anchor;
	int class_id;
	int order;
};

static int descending(const void *left, const void *right)
{
	const struct candidate *a = left, *b = right;
	if (a->score != b->score)
		return a->score > b->score ? -1 : 1;
	return (a->order > b->order) - (a->order < b->order);
}

static int read_exact(const char *path, int8_t *buffer, size_t size)
{
	FILE *file = fopen(path, "rb");
	if (!file) {
		fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
		return -1;
	}
	int valid = fread(buffer, 1, size, file) == size && fgetc(file) == EOF;
	fclose(file);
	if (!valid)
		fprintf(stderr, "invalid tensor byte count: %s\n", path);
	return valid ? 0 : -1;
}

static int number(const char *text, double *result)
{
	char *end;
	errno = 0;
	*result = strtod(text, &end);
	return errno || end == text || *end || !isfinite(*result) ? -1 : 0;
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts))
		return 0;
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

int main(int argc, char **argv)
{
	const char *directory = NULL;
	double score_zp = NAN, score_scale = NAN, box_zp = NAN, box_scale = NAN;
	double confidence = 0.25, runs = 10;
	char path[4096];
	int8_t *scores = NULL, *boxes = NULL;
	struct candidate *anchors = NULL, *candidates = NULL;
	uint64_t total_ns = 0;
	int status = 1;

	for (int i = 1; i < argc; i += 2) {
		if (i + 1 == argc)
			goto usage;
		if (!strcmp(argv[i], "--dump-dir")) {
			directory = argv[i + 1];
			continue;
		}
		double *value;
		if (!strcmp(argv[i], "--score-zp")) value = &score_zp;
		else if (!strcmp(argv[i], "--score-scale")) value = &score_scale;
		else if (!strcmp(argv[i], "--box-zp")) value = &box_zp;
		else if (!strcmp(argv[i], "--box-scale")) value = &box_scale;
		else if (!strcmp(argv[i], "--conf")) value = &confidence;
		else if (!strcmp(argv[i], "--runs")) value = &runs;
		else goto usage;
		if (number(argv[i + 1], value))
			goto usage;
	}
	if (!directory || !isfinite(score_zp) || !isfinite(box_zp) ||
	    !isfinite(score_scale) || !isfinite(box_scale) ||
	    score_zp < -128 || score_zp > 127 || box_zp < -128 || box_zp > 127 ||
	    score_zp != floor(score_zp) || box_zp != floor(box_zp) ||
	    score_scale <= 0 || box_scale <= 0 || confidence < 0 || confidence > 1 ||
	    runs < 1 || runs > 10000 || runs != floor(runs))
		goto usage;
	scores = malloc(2 * ANCHORS);
	boxes = malloc(4 * ANCHORS);
	anchors = malloc(ANCHORS * sizeof(*anchors));
	candidates = malloc(2 * TOPK * sizeof(*candidates));
	if (!scores || !boxes || !anchors || !candidates)
		goto cleanup;
	for (int index = 0; index < 2; ++index) {
		int length = snprintf(path, sizeof(path), "%s/output-%d.bin", directory, index);
		if (length < 0 || (size_t)length >= sizeof(path) ||
		    read_exact(path, index == 0 ? scores : boxes, (index == 0 ? 2 : 4) * ANCHORS))
			goto cleanup;
	}
	for (int run = 0; run < (int)runs; ++run) {
		uint64_t start = now_ns();
		for (int i = 0; i < ANCHORS; ++i) {
			int q = scores[i] > scores[ANCHORS + i] ? scores[i] : scores[ANCHORS + i];
			anchors[i] = (struct candidate){((float)q - (float)score_zp) * (float)score_scale, i, 0, i};
		}
		qsort(anchors, ANCHORS, sizeof(*anchors), descending);
		for (int i = 0; i < TOPK; ++i) {
			int anchor = anchors[i].anchor;
			for (int c = 0; c < 2; ++c)
				candidates[2 * i + c] = (struct candidate){
					((float)scores[c * ANCHORS + anchor] - (float)score_zp) * (float)score_scale,
					anchor, c, 2 * i + c};
		}
		qsort(candidates, 2 * TOPK, sizeof(*candidates), descending);
		total_ns += now_ns() - start;
	}
	int count = 0;
	printf("{\"confidence_threshold\":%.9g,\"detections\":[", confidence);
	for (int i = 0; i < TOPK; ++i) {
		const struct candidate *d = candidates + i;
		if (d->score < confidence)
			continue;
		float coordinates[4];
		for (int c = 0; c < 4; ++c)
			coordinates[c] = ((float)boxes[c * ANCHORS + d->anchor] - (float)box_zp) * (float)box_scale;
		printf("%s{\"xyxy\":[%.9g,%.9g,%.9g,%.9g],\"score\":%.9g,\"class_id\":%d}",
		       count ? "," : "", coordinates[0], coordinates[1], coordinates[2], coordinates[3], d->score, d->class_id);
		++count;
	}
	printf("]}\n");
	fprintf(stderr, "YOLO26_POSTPROCESS_OK runs=%d avg_elapsed_us=%llu detections=%d\n",
		(int)runs, (unsigned long long)(total_ns / 1000 / (int)runs), count);
	status = 0;
cleanup:
	free(scores); free(boxes); free(anchors); free(candidates);
	return status;
usage:
	fprintf(stderr, "usage: %s --dump-dir DIR --score-zp N --score-scale S --box-zp N --box-scale S [--conf 0.25] [--runs 10]\n"
		"Expected native int8 scores [1,2,8400] and xyxy boxes [1,4,8400]; query scales from this model's runtime.\n", argv[0]);
	return 2;
}
