/* Live adaptation of rknn_int8_postprocess.c; quantization comes from RKNN. */
#include "flow_yolov8.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REG_MAX 16
#define HEADS 3
#define CELLS 8400

struct raw_detection { float xyxy[4]; float score; unsigned int order; };
struct flow_yolov8 {
	rknn_tensor_attr attrs[FLOW_YOLOV8_OUTPUTS];
	struct raw_detection candidates[CELLS];
};

struct flow_yolov8 *flow_yolov8_create(const rknn_tensor_attr *attrs, size_t count)
{
	if (!attrs || count != FLOW_YOLOV8_OUTPUTS) return NULL;
	for (size_t i = 0; i < count; ++i) {
		int stride = 8 << (i / 2), size = FLOW_INPUT_WIDTH / stride;
		uint32_t channels = i % 2 ? 1 : 4 * REG_MAX;
		char name[64];
		snprintf(name, sizeof(name), i % 2 ? "helmet_scores_s%d" : "box_logits_s%d", stride);
		const rknn_tensor_attr *a = attrs + i;
		if (a->index != i || strcmp(a->name, name) || a->n_dims != 4 ||
		    a->dims[0] != 1 || a->dims[1] != channels ||
		    a->dims[2] != (uint32_t)size || a->dims[3] != (uint32_t)size ||
		    a->fmt != RKNN_TENSOR_NCHW || a->type != RKNN_TENSOR_INT8 ||
		    a->qnt_type != RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC ||
		    a->n_elems != channels * size * size || a->size != a->n_elems ||
		    !isfinite(a->scale) || a->scale <= 0 || a->zp < -128 || a->zp > 127)
			return NULL;
	}
	struct flow_yolov8 *decoder = malloc(sizeof(*decoder));
	if (decoder) memcpy(decoder->attrs, attrs, sizeof(decoder->attrs));
	return decoder;
}

static int descending(const void *left, const void *right)
{
	const struct raw_detection *a = left, *b = right;
	if (a->score != b->score) return a->score > b->score ? -1 : 1;
	return (a->order > b->order) - (a->order < b->order);
}

static float overlap(const struct raw_detection *a, const struct raw_detection *b)
{
	float w = fmaxf(0, fminf(a->xyxy[2], b->xyxy[2]) - fmaxf(a->xyxy[0], b->xyxy[0]));
	float h = fmaxf(0, fminf(a->xyxy[3], b->xyxy[3]) - fmaxf(a->xyxy[1], b->xyxy[1]));
	float area_a = (a->xyxy[2] - a->xyxy[0]) * (a->xyxy[3] - a->xyxy[1]);
	float area_b = (b->xyxy[2] - b->xyxy[0]) * (b->xyxy[3] - b->xyxy[1]);
	float total = area_a + area_b - w * h;
	return total > 0 ? w * h / total : 0;
}

static double unit(double value) { return fmin(fmax(value, 0), 1); }

int flow_yolov8_decode(struct flow_yolov8 *decoder, const rknn_output *outputs,
		       size_t count, const struct frame_metadata *metadata,
		       double confidence, double iou_threshold,
		       struct flow_detection detections[FLOW_MAX_DETECTIONS], size_t *detected)
{
	if (detected) *detected = 0;
	if (!decoder || !outputs || count != FLOW_YOLOV8_OUTPUTS || !metadata || !detections || !detected ||
	    !metadata->source_width || !metadata->source_height || !isfinite(metadata->resize_scale) ||
	    metadata->resize_scale <= 0 || metadata->pad_left < 0 || metadata->pad_top < 0 ||
	    metadata->pad_left >= FLOW_INPUT_WIDTH || metadata->pad_top >= FLOW_INPUT_HEIGHT ||
	    !isfinite(confidence) || confidence < 0 || confidence > 1 ||
	    !isfinite(iou_threshold) || iou_threshold < 0 || iou_threshold > 1)
		return -1;
	for (size_t i = 0; i < count; ++i)
		if (!outputs[i].buf || outputs[i].index != i || outputs[i].want_float ||
		    outputs[i].size != decoder->attrs[i].size) return -1;

	size_t candidates = 0;
	unsigned int order = 0;
	for (int head = 0; head < HEADS; ++head) {
		int stride = 8 << head, size = FLOW_INPUT_WIDTH / stride, area = size * size;
		const int8_t *boxes = outputs[2 * head].buf, *scores = outputs[2 * head + 1].buf;
		const rknn_tensor_attr *box_attr = decoder->attrs + 2 * head;
		const rknn_tensor_attr *score_attr = box_attr + 1;
		for (int cell = 0; cell < area; ++cell, ++order) {
			/* Exported score heads already contain sigmoid. */
			float score = fminf(fmaxf((scores[cell] - score_attr->zp) * score_attr->scale, 0), 1);
			if (score < confidence) continue;
			float distance[4];
			for (int side = 0; side < 4; ++side) {
				float logits[REG_MAX], maximum = -INFINITY, sum = 0, weighted = 0;
				for (int bin = 0; bin < REG_MAX; ++bin) {
					logits[bin] = (boxes[(side * REG_MAX + bin) * area + cell] - box_attr->zp) * box_attr->scale;
					maximum = fmaxf(maximum, logits[bin]);
				}
				for (int bin = 0; bin < REG_MAX; ++bin) {
					float probability = expf(logits[bin] - maximum);
					sum += probability;
					weighted += probability * bin;
				}
				distance[side] = weighted / sum * stride;
			}
			float x = (cell % size + 0.5f) * stride, y = (cell / size + 0.5f) * stride;
			decoder->candidates[candidates++] = (struct raw_detection){
				{x - distance[0], y - distance[1], x + distance[2], y + distance[3]}, score, order};
		}
	}
	qsort(decoder->candidates, candidates, sizeof(decoder->candidates[0]), descending);
	size_t selected = 0;
	for (size_t i = 0; i < candidates && selected < FLOW_MAX_DETECTIONS; ++i) {
		int suppressed = 0;
		for (size_t j = 0; j < selected; ++j)
			if (overlap(decoder->candidates + i, decoder->candidates + j) > iou_threshold) {
				suppressed = 1; break;
			}
		if (!suppressed) decoder->candidates[selected++] = decoder->candidates[i];
	}
	for (size_t i = 0; i < selected; ++i) {
		const struct raw_detection *raw = decoder->candidates + i;
		struct flow_detection d = {.score = raw->score, .class_id = 0};
		for (int corner = 0; corner < 4; ++corner) {
			double pad = corner % 2 ? metadata->pad_top : metadata->pad_left;
			double extent = corner % 2 ? metadata->source_height : metadata->source_width;
			d.xyxy[corner] = unit((raw->xyxy[corner] - pad) / metadata->resize_scale / extent);
		}
		if (d.xyxy[2] > d.xyxy[0] && d.xyxy[3] > d.xyxy[1]) detections[(*detected)++] = d;
	}
	return 0;
}

void flow_yolov8_destroy(struct flow_yolov8 *decoder) { free(decoder); }
