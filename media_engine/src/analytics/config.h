#ifndef ME_ANALYTICS_CONFIG_H
#define ME_ANALYTICS_CONFIG_H

#include "analytics/observation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ME_ANALYTICS_BACKEND_MAX 32U
#define ME_ANALYTICS_MODEL_MAX 96U
#define ME_ANALYTICS_EVIDENCE_MODE_MAX 24U

#define ME_ANALYTICS_DEFAULT_CONFIRM_FRAMES 3U
#define ME_ANALYTICS_DEFAULT_CONFIRM_MS 300U
#define ME_ANALYTICS_DEFAULT_DEBOUNCE_MS 300U
#define ME_ANALYTICS_DEFAULT_DISAPPEAR_GRACE_MS 1000U
#define ME_ANALYTICS_DEFAULT_COOLDOWN_MS 5000U
#define ME_ANALYTICS_DEFAULT_UPDATE_INTERVAL_MS 5000U
#define ME_ANALYTICS_DEFAULT_EVIDENCE_MAX_BYTES (512ULL * 1024ULL * 1024ULL)
#define ME_ANALYTICS_DEFAULT_EVIDENCE_RETENTION_S (72U * 60U * 60U)
#define ME_ANALYTICS_DEFAULT_EVIDENCE_JPEG_QUALITY 85U
#define ME_ANALYTICS_DEFAULT_EVENT_LOG_MAX_RECORDS 4096U
#define ME_ANALYTICS_DEFAULT_EVENT_LOG_MAX_BYTES (64ULL * 1024ULL * 1024ULL)

typedef struct {
	bool enabled;
	char backend[ME_ANALYTICS_BACKEND_MAX];
	char model[ME_ANALYTICS_MODEL_MAX];
	int width;
	int height;
	int fps;
	uint32_t score_threshold_q;
	uint32_t confirm_frames;
	uint32_t confirm_ms;
	uint32_t debounce_ms;
	uint32_t disappear_grace_ms;
	uint32_t cooldown_ms;
	uint32_t update_interval_ms;
	bool send_updates;
	bool send_end;

	bool roi_enabled;
	MeNormalizedBBox roi;
	bool line_enabled;
	uint32_t line_x1;
	uint32_t line_y1;
	uint32_t line_x2;
	uint32_t line_y2;
	char rule_id[ME_ANALYTICS_RULE_ID_MAX];
	MeRuleType rule_type;

	char evidence_mode[ME_ANALYTICS_EVIDENCE_MODE_MAX];
	uint64_t evidence_max_bytes;
	uint32_t evidence_retention_s;
	uint32_t evidence_jpeg_quality;
	uint32_t event_log_max_records;
	uint64_t event_log_max_bytes;
} MeAnalyticsConfig;

void me_analytics_config_defaults(MeAnalyticsConfig *config);
int me_analytics_config_validate(const MeAnalyticsConfig *config, char *err,
					 size_t errsz);

bool me_analytics_rule_type_parse(const char *value, MeRuleType *out);
bool me_analytics_evidence_mode_valid(const char *value);

#endif /* ME_ANALYTICS_CONFIG_H */
