#ifndef ME_GST_RUNNER_H
#define ME_GST_RUNNER_H

#include <stdbool.h>
#include <stddef.h>

#include "config/config.h"
#include "session/session.h"

typedef struct GstRunner GstRunner;

/* Event notification from the media plane (called on the GLib main loop
 * thread). event is "error", "eos" or "state"; session_id may be NULL. */
typedef void (*GstRunnerEventCb)(void *userdata, const char *event,
                                 const char *session_id, const char *message);

/* Creates the persistent capture pipeline
 * (v4l2src io-mode=dmabuf -> tee -> [kmssink preview]) and starts it.
 * Camera start failure is not fatal to the runner: IPC stays usable and
 * start_live reports the recorded failure. */
GstRunner *gst_runner_new(const EngineConfig *cfg, char *err, size_t errsz);
void gst_runner_free(GstRunner *r);

void gst_runner_set_event_cb(GstRunner *r, GstRunnerEventCb cb,
                             void *userdata);

/* Starts the RTP output branch for one session (V1: single session). */
int gst_runner_start_live(GstRunner *r, const SessionParams *p, char *err,
                          size_t errsz);
/* Stops the session branch, unlinks it from tee and releases the udpsink. */
int gst_runner_stop_live(GstRunner *r, const char *session_id, char *err,
                         size_t errsz);
int gst_runner_snapshot(GstRunner *r, const char *channel_id,
                        const char *out_dir, char *err, size_t errsz);

void gst_runner_status(GstRunner *r, bool *running, int *fps, int *bitrate);

#endif /* ME_GST_RUNNER_H */
