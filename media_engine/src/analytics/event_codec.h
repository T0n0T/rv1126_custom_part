#ifndef ME_ANALYTICS_EVENT_CODEC_H
#define ME_ANALYTICS_EVENT_CODEC_H

#include "analytics/event_engine.h"

#include <stddef.h>

typedef struct cJSON cJSON;

/* The same event object is used by the durable journal and the IPC bridge. */
cJSON *me_analytics_event_to_json(const MeAnalyticsEvent *event);
int me_analytics_event_from_json(const cJSON *object,
                                 MeAnalyticsEvent *event, char *err,
                                 size_t errsz);

#endif /* ME_ANALYTICS_EVENT_CODEC_H */
