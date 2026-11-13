#ifndef FLOW_YOLOV8_H
#define FLOW_YOLOV8_H

#include "flow_image.h"
#include "rknn_api.h"

#define FLOW_YOLOV8_OUTPUTS 6

struct flow_yolov8;

/* Accept only the exported single-class 640x640 NCHW INT8 DFL heads. */
struct flow_yolov8 *flow_yolov8_create(const rknn_tensor_attr *attrs, size_t count);
/* Decode using the queried quantization, NMS in model space, then unletterbox. */
int flow_yolov8_decode(struct flow_yolov8 *decoder, const rknn_output *outputs,
		       size_t count, const struct frame_metadata *metadata,
		       double confidence, double iou_threshold,
		       struct flow_detection detections[FLOW_MAX_DETECTIONS], size_t *detected);
void flow_yolov8_destroy(struct flow_yolov8 *decoder);

#endif
