/* Feed saved native tensors through the same decoder used by the live app. */
#include "flow_yolov8.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv)
{
	if (argc != 11) return 2;
	rknn_tensor_attr attrs[FLOW_YOLOV8_OUTPUTS] = {{0}};
	rknn_output outputs[FLOW_YOLOV8_OUTPUTS] = {{0}};
	FILE *config = fopen(argv[2], "r");
	if (!config) return 2;
	for (int head = 0; head < 3; ++head) {
		int stride = 8 << head, size = 640 / stride;
		int box_zp, score_zp;
		float box_scale, score_scale;
		if (fscanf(config, "%d %f %d %f", &box_zp, &box_scale, &score_zp, &score_scale) != 4) return 2;
		for (int kind = 0; kind < 2; ++kind) {
			int i = head * 2 + kind;
			rknn_tensor_attr *a = attrs + i;
			a->index = (uint32_t)i;
			snprintf(a->name, sizeof(a->name), kind ? "helmet_scores_s%d" : "box_logits_s%d", stride);
			a->n_dims = 4; a->dims[0] = 1; a->dims[1] = kind ? 1 : 64;
			a->dims[2] = a->dims[3] = (uint32_t)size;
			a->size = a->n_elems = a->dims[1] * size * size;
			a->fmt = RKNN_TENSOR_NCHW; a->type = RKNN_TENSOR_INT8;
			a->qnt_type = RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC;
			a->zp = kind ? score_zp : box_zp; a->scale = kind ? score_scale : box_scale;
			outputs[i].index = (uint32_t)i; outputs[i].size = a->size;
			outputs[i].buf = malloc(a->size);
			char path[4096];
			snprintf(path, sizeof(path), "%s/output-%d.bin", argv[1], i);
			FILE *input = fopen(path, "rb");
			if (!input || !outputs[i].buf) return 2;
			outputs[i].size = (uint32_t)fread(outputs[i].buf, 1, a->size, input);
			fclose(input);
		}
	}
	fclose(config);
	/* Exercise rejection at the runtime contract boundary. */
	switch (atoi(argv[10])) {
	case 1: attrs[0].dims[1] = 32; break;
	case 2: attrs[1].fmt = RKNN_TENSOR_NHWC; break;
	case 3: attrs[2].type = RKNN_TENSOR_FLOAT16; break;
	case 4: strcpy(attrs[3].name, "unknown_scores"); break;
	case 5: attrs[4].scale = 0; break;
	}
	struct frame_metadata metadata = {.source_width = (uint32_t)atoi(argv[3]),
		.source_height = (uint32_t)atoi(argv[4]), .resize_scale = atof(argv[5]),
		.pad_left = atoi(argv[6]), .pad_top = atoi(argv[7])};
	struct flow_yolov8 *decoder = flow_yolov8_create(attrs, FLOW_YOLOV8_OUTPUTS);
	struct flow_detection detections[FLOW_MAX_DETECTIONS];
	size_t count = 0;
	int status = decoder ? flow_yolov8_decode(decoder, outputs, FLOW_YOLOV8_OUTPUTS,
		&metadata, atof(argv[8]), atof(argv[9]), detections, &count) : -1;
	if (status == 0) {
		printf("[");
		for (size_t i = 0; i < count; ++i) {
			const struct flow_detection *d = detections + i;
			printf("%s{\"xyxy\":[%.9g,%.9g,%.9g,%.9g],\"score\":%.9g,\"class_id\":%u}",
				i ? "," : "", d->xyxy[0], d->xyxy[1], d->xyxy[2], d->xyxy[3], d->score, d->class_id);
		}
		puts("]");
	}
	for (int i = 0; i < FLOW_YOLOV8_OUTPUTS; ++i) free(outputs[i].buf);
	flow_yolov8_destroy(decoder);
	return status == 0 ? 0 : 1;
}
