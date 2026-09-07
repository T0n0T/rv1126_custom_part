#ifndef ME_ANALYTICS_ROCKIVA_RUNNER_H
#define ME_ANALYTICS_ROCKIVA_RUNNER_H

#include "analytics/config.h"
#include "analytics/observation.h"

#include <gst/app/gstappsink.h>

#include <stddef.h>
#include <stdint.h>

typedef struct MeRockivaRunner MeRockivaRunner;

typedef void (*MeRockivaObservationCb)(void *userdata,
                                       const MeNormalizedObservation *observation);

/* Creates a RockIVA detector attached to an already-built appsink. The
 * appsink callback is installed before the capture pipeline is started. */
MeRockivaRunner *me_rockiva_runner_new(
    GstAppSink *appsink, const MeAnalyticsConfig *config,
    MeRockivaObservationCb observation_cb, void *observation_userdata,
    char *err, size_t errsz);

/* Changes the logical stream associated with subsequently submitted frames. */
void me_rockiva_runner_set_stream(MeRockivaRunner *runner,
                                  const char *channel_id,
                                  uint64_t stream_epoch);

/* Stops callbacks, waits for SDK-owned frames, and releases the detector. */
int me_rockiva_runner_free(MeRockivaRunner *runner);

#endif /* ME_ANALYTICS_ROCKIVA_RUNNER_H */
