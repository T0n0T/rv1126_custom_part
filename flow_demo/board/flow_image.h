#ifndef FLOW_IMAGE_H
#define FLOW_IMAGE_H

#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define FLOW_INPUT_WIDTH 640
#define FLOW_INPUT_HEIGHT 640
#define FLOW_INPUT_BYTES ((size_t)FLOW_INPUT_WIDTH * FLOW_INPUT_HEIGHT * 3)
#define FLOW_MAX_DETECTIONS 300

/* Live coordinates describe the image after the configured camera rotation. */
struct frame_metadata {
	int64_t frame_index;
	double pts_ms;
	uint32_t source_width;
	uint32_t source_height;
	double resize_scale;
	int32_t pad_left;
	int32_t pad_top;
};

struct flow_detection {
	double xyxy[4];
	float score;
	uint32_t class_id;
};

#define FLOW_ZONE_VERTICES 32
struct flow_polygon { size_t count; double xy[FLOW_ZONE_VERTICES][2]; };
struct flow_track_view {
	struct flow_detection detection;
	int track_id;
	int zone; /* 0 unknown, 1 inside, 2 transition, 3 outside */
	int pending;
};
struct flow_overlay {
	int enabled;
	int geometry_confirmed;
	int take_out;
	int put_in;
	int uncertain;
	int pending;
	double pts_ms;
	double detector_fps;
	double infer_ms;
	size_t count;
	struct flow_track_view tracks[FLOW_MAX_DETECTIONS];
	struct flow_polygon zones[3];
	char last_event[64];
	char reason[80];
};

int flow_letterbox_rgb(const uint8_t *source, int width, int height, int stride,
		       int rotation, uint8_t *input, struct frame_metadata *metadata);
int flow_letterbox_bgr(const uint8_t *source, int width, int height, int stride,
		       int rotation, uint8_t *input, struct frame_metadata *metadata);
void flow_render_bgrx(const uint8_t *input, const struct frame_metadata *metadata,
		      const struct flow_detection *detections, size_t count,
		      uint8_t *screen, int width, int height, double fps, double infer_ms);
int flow_save_ppm(const char *path, const uint8_t *screen, int width, int height);
int flow_save_rotated_ppm(const char *path, const uint8_t *source, int width, int height, int stride, int rotation);
int flow_save_rotated_bgr_ppm(const char *path, const uint8_t *source, int width, int height, int stride, int rotation);
void flow_draw_overlay(uint8_t *screen, int width, int height, int stride,
		       int source_width, int source_height,
		       const struct flow_overlay *overlay, double preview_fps, double pts_ms);
/* Draw logical UI coordinates directly into the rotated native panel buffer. */
int flow_draw_overlay_rotated(uint8_t *screen, int width, int height, int stride,
		       int screen_rotation, int source_width, int source_height,
		       const struct flow_overlay *overlay, double preview_fps, double pts_ms);

#ifdef __cplusplus
}
#endif

#endif
