#ifndef FLOW_CAMERA_H
#define FLOW_CAMERA_H

#include "flow_image.h"
#include <signal.h>

struct flow_camera_options {
	const char *device;
	const char *iq_dir;
	const char *snapshot_path;
	const char *source_snapshot_path;
	const char *stats_path;
	int camera_id;
	int width;
	int height;
	int fps;
	int rotation;
	int screen_rotation; /* Clockwise panel transform; independent of camera coordinates. */
	int input_bgr; /* ISP RGB24 port byte order, verified against NV12 on this board. */
	int connector_id;
	int plane_id;
	int screen_width;
	int screen_height;
	int no_display;
};

struct flow_camera;

struct flow_camera *flow_camera_open(const struct flow_camera_options *options);
/* 1 = a frame, 0 = interrupted, -1 = capture/PTS/preprocessing failure. */
int flow_camera_next(struct flow_camera *camera, uint8_t *input,
		     struct frame_metadata *metadata, const volatile sig_atomic_t *stop);
int flow_camera_present(struct flow_camera *camera, const uint8_t *input,
			const struct frame_metadata *metadata,
			const struct flow_detection *detections, size_t count,
			double infer_ms, double post_ms, const struct flow_overlay *overlay);
/* Drain display, report counters, stop capture/AIQ/KMS, release all resources. */
int flow_camera_close(struct flow_camera *camera);

#endif
