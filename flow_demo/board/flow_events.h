#ifndef FLOW_EVENTS_H
#define FLOW_EVENTS_H
#include "flow_image.h"
#ifdef __cplusplus
extern "C" {
#endif
struct flow_events;
struct flow_events *flow_events_open(const char *zones_path, const char *session_id,
	const char *output_directory, int rotation, int source_width, int source_height,
	int allow_debug);
int flow_events_update(struct flow_events *events, const struct frame_metadata *metadata,
	const struct flow_detection *detections, size_t count, struct flow_overlay *overlay);
/* run_ok is false on capture, display, inference or output failure. */
int flow_events_close(struct flow_events *events, int run_ok);
#ifdef __cplusplus
}
#endif
#endif
