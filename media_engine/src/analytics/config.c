#include "analytics/config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

static void analytics_config_set_err(char *err, size_t errsz, const char *fmt,
				     ...)
{
	va_list ap;

	if (!err || errsz == 0)
		return;
	va_start(ap, fmt);
	vsnprintf(err, errsz, fmt, ap);
	va_end(ap);
}

static bool bounded_string(const char *value, size_t cap)
{
	return value && memchr(value, '\0', cap) != NULL && value[0] != '\0';
}

static bool valid_rect(const MeNormalizedBBox *rect)
{
	return rect && rect->left < rect->right && rect->top < rect->bottom &&
	       rect->right <= ME_ANALYTICS_COORD_SCALE &&
	       rect->bottom <= ME_ANALYTICS_COORD_SCALE;
}

static bool valid_rule_type(MeRuleType type)
{
	return type >= ME_RULE_TYPE_OCCUPANCY && type <= ME_RULE_TYPE_INTRUSION;
}

void me_analytics_config_defaults(MeAnalyticsConfig *config)
{
	if (!config)
		return;
	memset(config, 0, sizeof(*config));
	snprintf(config->backend, sizeof(config->backend), "rockiva");
	config->confirm_frames = ME_ANALYTICS_DEFAULT_CONFIRM_FRAMES;
	config->confirm_ms = ME_ANALYTICS_DEFAULT_CONFIRM_MS;
	config->debounce_ms = ME_ANALYTICS_DEFAULT_DEBOUNCE_MS;
	config->disappear_grace_ms = ME_ANALYTICS_DEFAULT_DISAPPEAR_GRACE_MS;
	config->cooldown_ms = ME_ANALYTICS_DEFAULT_COOLDOWN_MS;
	config->update_interval_ms = ME_ANALYTICS_DEFAULT_UPDATE_INTERVAL_MS;
	snprintf(config->rule_id, sizeof(config->rule_id), "people-flow");
	config->rule_type = ME_RULE_TYPE_OCCUPANCY;
	snprintf(config->evidence_mode, sizeof(config->evidence_mode),
		 "stock_wvp_pull");
	config->evidence_max_bytes = ME_ANALYTICS_DEFAULT_EVIDENCE_MAX_BYTES;
	config->evidence_retention_s = ME_ANALYTICS_DEFAULT_EVIDENCE_RETENTION_S;
	config->evidence_jpeg_quality = ME_ANALYTICS_DEFAULT_EVIDENCE_JPEG_QUALITY;
	config->event_log_max_records = ME_ANALYTICS_DEFAULT_EVENT_LOG_MAX_RECORDS;
	config->event_log_max_bytes = ME_ANALYTICS_DEFAULT_EVENT_LOG_MAX_BYTES;
}

int me_analytics_config_validate(const MeAnalyticsConfig *config, char *err,
					 size_t errsz)
{
	if (!config) {
		analytics_config_set_err(err, errsz, "analytics config is null");
		return -1;
	}
	if (!bounded_string(config->backend, sizeof(config->backend))) {
		analytics_config_set_err(err, errsz,
					 "analytics backend is missing or overlong");
		return -1;
	}
	if (config->width < 0 || config->height < 0 || config->fps < 0) {
		analytics_config_set_err(err, errsz,
					 "analytics dimensions and fps cannot be negative");
		return -1;
	}
	if (config->enabled && (config->width < 2 || config->height < 2 ||
				       (config->width & 1) || (config->height & 1) ||
				       config->fps <= 0)) {
		analytics_config_set_err(err, errsz,
					 "enabled analytics needs positive even width/height and fps");
		return -1;
	}
	if (config->width == 1 || config->height == 1 ||
	    (config->width > 0 && (config->width & 1)) ||
	    (config->height > 0 && (config->height & 1))) {
		analytics_config_set_err(err, errsz,
					 "analytics dimensions must be even when configured");
		return -1;
	}
	if (config->score_threshold_q > ME_ANALYTICS_COORD_SCALE) {
		analytics_config_set_err(err, errsz,
					 "analytics score threshold must be 0..%u",
					 ME_ANALYTICS_COORD_SCALE);
		return -1;
	}
	if (config->confirm_frames == 0 || config->confirm_ms == 0 ||
	    config->update_interval_ms == 0) {
		analytics_config_set_err(err, errsz,
					 "analytics confirmation and update intervals must be positive");
		return -1;
	}
	if (config->roi_enabled && !valid_rect(&config->roi)) {
		analytics_config_set_err(err, errsz, "analytics ROI is invalid");
		return -1;
	}
	if (config->line_enabled &&
	    (config->line_x1 > ME_ANALYTICS_COORD_SCALE ||
	     config->line_y1 > ME_ANALYTICS_COORD_SCALE ||
	     config->line_x2 > ME_ANALYTICS_COORD_SCALE ||
	     config->line_y2 > ME_ANALYTICS_COORD_SCALE ||
	     (config->line_x1 == config->line_x2 &&
	      config->line_y1 == config->line_y2))) {
		analytics_config_set_err(err, errsz, "analytics line is invalid");
		return -1;
	}
	if (!bounded_string(config->rule_id, sizeof(config->rule_id)) ||
	    !valid_rule_type(config->rule_type)) {
		analytics_config_set_err(err, errsz, "analytics rule is invalid");
		return -1;
	}
	if (!me_analytics_evidence_mode_valid(config->evidence_mode)) {
		analytics_config_set_err(err, errsz,
					 "analytics evidence mode must be stock_wvp_pull or exact_evidence");
		return -1;
	}
	if (config->evidence_max_bytes == 0 || config->evidence_retention_s == 0 ||
	    config->evidence_jpeg_quality < 1 ||
	    config->evidence_jpeg_quality > 100 ||
	    config->event_log_max_records == 0 || config->event_log_max_bytes == 0) {
		analytics_config_set_err(err, errsz,
					 "analytics evidence and event log limits are invalid");
		return -1;
	}
	if (config->enabled && !bounded_string(config->model, sizeof(config->model))) {
		analytics_config_set_err(err, errsz,
					 "enabled analytics needs a model selected by board validation");
		return -1;
	}
	return 0;
}

bool me_analytics_rule_type_parse(const char *value, MeRuleType *out)
{
	if (!value || !out)
		return false;
	if (!strcasecmp(value, "occupancy"))
		*out = ME_RULE_TYPE_OCCUPANCY;
	else if (!strcasecmp(value, "line_cross") ||
		 !strcasecmp(value, "line-cross"))
		*out = ME_RULE_TYPE_LINE_CROSS;
	else if (!strcasecmp(value, "intrusion"))
		*out = ME_RULE_TYPE_INTRUSION;
	else
		return false;
	return true;
}

bool me_analytics_evidence_mode_valid(const char *value)
{
	return value && (!strcasecmp(value, "stock_wvp_pull") ||
			 !strcasecmp(value, "exact_evidence"));
}
