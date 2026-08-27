#include "config/config.h"

#include "common/util.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <yaml.h>

#define ME_CFG_DEFAULT_FILE "/oem/usr/share/media_engine.yaml"

static bool parse_bool_value(const char *s, bool *out)
{
	if (!s || !out)
		return false;
	if (!strcmp(s, "1") || !strcasecmp(s, "on") || !strcasecmp(s, "true") ||
	    !strcasecmp(s, "yes")) {
		*out = true;
		return true;
	}
	if (!strcmp(s, "0") || !strcasecmp(s, "off") || !strcasecmp(s, "false") ||
	    !strcasecmp(s, "no")) {
		*out = false;
		return true;
	}
	return false;
}

static int parse_int_value(const char *s, int *out, const char *key, char *err,
                           size_t errsz)
{
	char *end = NULL;
	long v;

	if (!s || !*s) {
		me_set_err(err, errsz, "config %s: empty value", key);
		return -1;
	}
	errno = 0;
	v = strtol(s, &end, 10);
	if (errno || !end || *end != '\0' || v < INT_MIN || v > INT_MAX) {
		me_set_err(err, errsz, "config %s: invalid integer \"%s\"", key, s);
		return -1;
	}
	*out = (int)v;
	return 0;
}

static int parse_u32_value(const char *s, uint32_t *out, const char *key,
                           char *err, size_t errsz)
{
	char *end = NULL;
	unsigned long long v;

	if (!s || !*s || s[0] == '-' || s[0] == '+') {
		me_set_err(err, errsz, "config %s: empty value", key);
		return -1;
	}
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || !end || *end != '\0' || v > UINT32_MAX) {
		me_set_err(err, errsz, "config %s: invalid unsigned integer \"%s\"",
		           key, s);
		return -1;
	}
	*out = (uint32_t)v;
	return 0;
}

static int parse_u64_value(const char *s, uint64_t *out, const char *key,
                           char *err, size_t errsz)
{
	char *end = NULL;
	unsigned long long v;

	if (!s || !*s || s[0] == '-' || s[0] == '+') {
		me_set_err(err, errsz, "config %s: empty value", key);
		return -1;
	}
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno || !end || *end != '\0') {
		me_set_err(err, errsz, "config %s: invalid unsigned integer \"%s\"",
		           key, s);
		return -1;
	}
	*out = (uint64_t)v;
	return 0;
}

static int set_string(char *dst, size_t cap, const char *value,
                      const char *key, const char *path, int line, char *err,
                      size_t errsz)
{
	if (!dst || cap == 0 || !value) {
		me_set_err(err, errsz, "%s:%d: invalid destination for \"%s\"",
		           path ? path : "<config>", line, key ? key : "value");
		return -1;
	}
	size_t len = strlen(value);

	if (len >= cap) {
		me_set_err(err, errsz, "%s:%d: value for \"%s\" too long (max %zu)",
		           path, line, key, cap - 1);
		return -1;
	}
	memcpy(dst, value, len + 1);
	return 0;
}

void engine_config_defaults(EngineConfig *cfg)
{
	if (!cfg)
		return;
	memset(cfg, 0, sizeof(*cfg));
	cfg->cam_id = 0;
	snprintf(cfg->iq_dir, sizeof(cfg->iq_dir), "%s", ME_CFG_DEFAULT_IQ_DIR);
	snprintf(cfg->device, sizeof(cfg->device), "%s", ME_CFG_DEFAULT_DEVICE);
	snprintf(cfg->format, sizeof(cfg->format), "%s", ME_CFG_DEFAULT_FORMAT);
	cfg->width = ME_CFG_DEFAULT_WIDTH;
	cfg->height = ME_CFG_DEFAULT_HEIGHT;
	cfg->fps = ME_CFG_DEFAULT_FPS;
	cfg->connector_id = ME_CFG_DEFAULT_CONNECTOR;
	cfg->plane_id = ME_CFG_DEFAULT_PLANE;
	cfg->preview = true;
	cfg->preview_rotation = 0;
	cfg->preview_width = ME_CFG_DEFAULT_PREVIEW_WIDTH;
	cfg->preview_height = ME_CFG_DEFAULT_PREVIEW_HEIGHT;
	cfg->stream_rotation = 0;
	snprintf(cfg->af_mode, sizeof(cfg->af_mode), "off");
	snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s",
	         ME_CFG_DEFAULT_SOCKET);
	snprintf(cfg->snapshot_dir, sizeof(cfg->snapshot_dir), "%s",
	         ME_CFG_DEFAULT_SNAPSHOT_DIR);
	me_analytics_config_defaults(&cfg->analytics);
}

int engine_config_validate(const EngineConfig *cfg, char *err, size_t errsz)
{
	if (!cfg) {
		me_set_err(err, errsz, "engine config is null");
		return -1;
	}
	return me_analytics_config_validate(&cfg->analytics, err, errsz);
}

static bool is_known_key(const char *key)
{
	static const char *const known[] = {
	    "cam_id",       "iq_dir",     "device",   "format",
	    "width",        "height",     "fps",      "connector_id",
	    "plane_id",     "preview",    "preview_rotation", "stream_rotation",
	    "preview_width", "preview_height", "af_mode", "socket",
	    "socket_path",  "snapshot_dir", "analytics_enabled",
	    "analytics_backend", "analytics_model", "analytics_width",
	    "analytics_height", "analytics_fps", "analytics_score_threshold_q",
	    "analytics_confirm_frames", "analytics_confirm_ms",
	    "analytics_debounce_ms", "analytics_disappear_grace_ms",
	    "analytics_cooldown_ms", "analytics_update_interval_ms",
	    "analytics_send_updates", "analytics_send_end", "analytics_roi_enabled",
	    "analytics_roi_left", "analytics_roi_top", "analytics_roi_right",
	    "analytics_roi_bottom", "analytics_line_enabled", "analytics_line_x1",
	    "analytics_line_y1", "analytics_line_x2", "analytics_line_y2",
	    "analytics_rule_id", "analytics_rule_type", "analytics_evidence_mode",
	    "analytics_evidence_max_bytes", "analytics_evidence_retention_s",
	    "analytics_evidence_jpeg_quality", "analytics_event_log_max_records",
	    "analytics_event_log_max_bytes",
	};
	size_t i;

	for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
		if (!strcmp(key, known[i]))
			return true;
	}
	return false;
}

static int config_apply_document(EngineConfig *cfg, yaml_document_t *doc,
                                 const char *path, char *err, size_t errsz)
{
	yaml_node_t *root;
	yaml_node_pair_t *pair;

	if (!cfg || !doc || !path) {
		me_set_err(err, errsz, "invalid config document arguments");
		return -1;
	}
	root = yaml_document_get_root_node(doc);
	if (!root)
		return 0; /* empty document */
	if (root->type != YAML_MAPPING_NODE) {
		me_set_err(err, errsz, "%s:%d: config root must be a mapping", path,
		           (int)root->start_mark.line + 1);
		return -1;
	}

	for (pair = root->data.mapping.pairs.start;
	     pair < root->data.mapping.pairs.top; pair++) {
		yaml_node_t *knode = yaml_document_get_node(doc, pair->key);
		yaml_node_t *vnode = yaml_document_get_node(doc, pair->value);
		char key[64];
		char value[512];
		bool b;

		if (!knode || knode->type != YAML_SCALAR_NODE) {
			me_set_err(err, errsz, "%s:%d: mapping keys must be scalars",
			           path, knode ? (int)knode->start_mark.line + 1 : 0);
			return -1;
		}
		snprintf(key, sizeof(key), "%.63s",
		         (const char *)knode->data.scalar.value);

		if (!vnode) {
			me_set_err(err, errsz, "%s:%d: dangling alias for key \"%s\"",
			           path, (int)knode->start_mark.line + 1, key);
			return -1;
		}
		if (vnode->type != YAML_SCALAR_NODE) {
			if (is_known_key(key)) {
				me_set_err(err, errsz,
				           "%s:%d: value for \"%s\" must be a scalar", path,
				           (int)vnode->start_mark.line + 1, key);
				return -1;
			}
			me_log(ME_LOG_WARN,
			       "config %s:%d: ignoring complex value for unknown key "
			       "\"%s\"",
			       path, (int)vnode->start_mark.line + 1, key);
			continue;
		}
		if (!vnode->data.scalar.value ||
		    vnode->data.scalar.length >= sizeof(value)) {
			me_set_err(err, errsz,
			           "%s:%d: empty or over-long value for \"%s\"", path,
			           (int)vnode->start_mark.line + 1, key);
			return -1;
		}
		snprintf(value, sizeof(value), "%.*s",
		         (int)vnode->data.scalar.length,
		         (const char *)vnode->data.scalar.value);
		me_trim(value);
		if (!*value) {
			me_set_err(err, errsz, "%s:%d: empty value for \"%s\"", path,
			           (int)vnode->start_mark.line + 1, key);
			return -1;
		}

		if (!strcmp(key, "cam_id")) {
			if (parse_int_value(value, &cfg->cam_id, key, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "iq_dir")) {
			if (set_string(cfg->iq_dir, sizeof(cfg->iq_dir), value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "device")) {
			if (set_string(cfg->device, sizeof(cfg->device), value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "format")) {
			if (set_string(cfg->format, sizeof(cfg->format), value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "width")) {
			if (parse_int_value(value, &cfg->width, key, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "height")) {
			if (parse_int_value(value, &cfg->height, key, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "fps")) {
			if (parse_int_value(value, &cfg->fps, key, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "connector_id")) {
			if (parse_int_value(value, &cfg->connector_id, key, err, errsz) !=
			    0)
				return -1;
		} else if (!strcmp(key, "plane_id")) {
			if (parse_int_value(value, &cfg->plane_id, key, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "preview")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz, "%s:%d: preview must be on/off",
				           path, (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->preview = b;
		} else if (!strcmp(key, "preview_rotation")) {
			if (parse_int_value(value, &cfg->preview_rotation, key, err,
			                    errsz) != 0)
				return -1;
			if (cfg->preview_rotation != 0 && cfg->preview_rotation != 90 &&
			    cfg->preview_rotation != 180 &&
			    cfg->preview_rotation != 270) {
				me_set_err(err, errsz, "%s:%d: preview_rotation must be "
				           "0/90/180/270",
				           path, (int)knode->start_mark.line + 1);
				return -1;
			}
		} else if (!strcmp(key, "stream_rotation")) {
			if (parse_int_value(value, &cfg->stream_rotation, key, err,
			                    errsz) != 0)
				return -1;
			if (cfg->stream_rotation != 0 && cfg->stream_rotation != 90 &&
			    cfg->stream_rotation != 180 &&
			    cfg->stream_rotation != 270) {
				me_set_err(err, errsz, "%s:%d: stream_rotation must be "
				           "0/90/180/270",
				           path, (int)knode->start_mark.line + 1);
				return -1;
			}
		} else if (!strcmp(key, "preview_width")) {
			if (parse_int_value(value, &cfg->preview_width, key, err,
			                    errsz) != 0)
				return -1;
			if (cfg->preview_width < 2 || (cfg->preview_width & 1)) {
				me_set_err(err, errsz, "%s:%d: preview_width must be "
				           "an even number >= 2",
				           path, (int)knode->start_mark.line + 1);
				return -1;
			}
		} else if (!strcmp(key, "preview_height")) {
			if (parse_int_value(value, &cfg->preview_height, key, err,
			                    errsz) != 0)
				return -1;
			if (cfg->preview_height < 2 || (cfg->preview_height & 1)) {
				me_set_err(err, errsz, "%s:%d: preview_height must be "
				           "an even number >= 2",
				           path, (int)knode->start_mark.line + 1);
				return -1;
			}
		} else if (!strcmp(key, "af_mode")) {
			if (set_string(cfg->af_mode, sizeof(cfg->af_mode), value, key,
			               path, (int)knode->start_mark.line + 1, err,
			               errsz) != 0)
				return -1;
		} else if (!strcmp(key, "socket") ||
		           !strcmp(key, "socket_path")) {
			if (set_string(cfg->socket_path, sizeof(cfg->socket_path), value,
			               key, path, (int)knode->start_mark.line + 1, err,
			               errsz) != 0)
				return -1;
		} else if (!strcmp(key, "snapshot_dir")) {
			if (set_string(cfg->snapshot_dir, sizeof(cfg->snapshot_dir),
			               value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_enabled")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_enabled must be on/off", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->analytics.enabled = b;
		} else if (!strcmp(key, "analytics_backend")) {
			if (set_string(cfg->analytics.backend,
			               sizeof(cfg->analytics.backend), value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_model")) {
			if (set_string(cfg->analytics.model, sizeof(cfg->analytics.model),
			               value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_width")) {
			if (parse_int_value(value, &cfg->analytics.width, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_height")) {
			if (parse_int_value(value, &cfg->analytics.height, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_fps")) {
			if (parse_int_value(value, &cfg->analytics.fps, key, err, errsz) !=
			    0)
				return -1;
		} else if (!strcmp(key, "analytics_score_threshold_q")) {
			if (parse_u32_value(value, &cfg->analytics.score_threshold_q, key,
			                    err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_confirm_frames")) {
			if (parse_u32_value(value, &cfg->analytics.confirm_frames, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_confirm_ms")) {
			if (parse_u32_value(value, &cfg->analytics.confirm_ms, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_debounce_ms")) {
			if (parse_u32_value(value, &cfg->analytics.debounce_ms, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_disappear_grace_ms")) {
			if (parse_u32_value(value, &cfg->analytics.disappear_grace_ms, key,
			                    err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_cooldown_ms")) {
			if (parse_u32_value(value, &cfg->analytics.cooldown_ms, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_update_interval_ms")) {
			if (parse_u32_value(value, &cfg->analytics.update_interval_ms, key,
			                    err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_send_updates")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_send_updates must be on/off", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->analytics.send_updates = b;
		} else if (!strcmp(key, "analytics_send_end")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_send_end must be on/off", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->analytics.send_end = b;
		} else if (!strcmp(key, "analytics_roi_enabled")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_roi_enabled must be on/off", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->analytics.roi_enabled = b;
		} else if (!strcmp(key, "analytics_roi_left")) {
			if (parse_u32_value(value, &cfg->analytics.roi.left, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_roi_top")) {
			if (parse_u32_value(value, &cfg->analytics.roi.top, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_roi_right")) {
			if (parse_u32_value(value, &cfg->analytics.roi.right, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_roi_bottom")) {
			if (parse_u32_value(value, &cfg->analytics.roi.bottom, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_line_enabled")) {
			if (!parse_bool_value(value, &b)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_line_enabled must be on/off", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			cfg->analytics.line_enabled = b;
		} else if (!strcmp(key, "analytics_line_x1")) {
			if (parse_u32_value(value, &cfg->analytics.line_x1, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_line_y1")) {
			if (parse_u32_value(value, &cfg->analytics.line_y1, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_line_x2")) {
			if (parse_u32_value(value, &cfg->analytics.line_x2, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_line_y2")) {
			if (parse_u32_value(value, &cfg->analytics.line_y2, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_rule_id")) {
			if (set_string(cfg->analytics.rule_id,
			               sizeof(cfg->analytics.rule_id), value, key, path,
			               (int)knode->start_mark.line + 1, err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_rule_type")) {
			if (!me_analytics_rule_type_parse(value,
			                                  &cfg->analytics.rule_type)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_rule_type is invalid", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
		} else if (!strcmp(key, "analytics_evidence_mode")) {
			if (!me_analytics_evidence_mode_valid(value)) {
				me_set_err(err, errsz,
				           "%s:%d: analytics_evidence_mode is invalid", path,
				           (int)knode->start_mark.line + 1);
				return -1;
			}
			if (set_string(cfg->analytics.evidence_mode,
			               sizeof(cfg->analytics.evidence_mode), value, key,
			               path, (int)knode->start_mark.line + 1, err,
			               errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_evidence_max_bytes")) {
			if (parse_u64_value(value, &cfg->analytics.evidence_max_bytes, key,
			                    err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_evidence_retention_s")) {
			if (parse_u32_value(value, &cfg->analytics.evidence_retention_s, key,
			                    err, errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_evidence_jpeg_quality")) {
			if (parse_u32_value(value,
			                    &cfg->analytics.evidence_jpeg_quality, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_event_log_max_records")) {
			if (parse_u32_value(value,
			                    &cfg->analytics.event_log_max_records, key, err,
			                    errsz) != 0)
				return -1;
		} else if (!strcmp(key, "analytics_event_log_max_bytes")) {
			if (parse_u64_value(value, &cfg->analytics.event_log_max_bytes, key,
			                    err, errsz) != 0)
				return -1;
		} else {
			me_log(ME_LOG_WARN, "config %s:%d: unknown key \"%s\" ignored",
			       path, (int)knode->start_mark.line + 1, key);
		}
	}
	return 0;
}

int engine_config_load(EngineConfig *cfg, const char *path, char *err,
                       size_t errsz)
{
	yaml_parser_t parser;
	yaml_document_t doc;
	FILE *f;
	int rc = -1;
	int loaded;

	if (!cfg) {
		me_set_err(err, errsz, "engine config is null");
		return -1;
	}
	if (!path)
		path = ME_CFG_DEFAULT_FILE;
	f = fopen(path, "r");
	if (!f) {
		if (errno == ENOENT) {
			me_log(ME_LOG_INFO, "no config file %s, using defaults", path);
			return 0;
		}
		me_set_err(err, errsz, "open config %s: %s", path, strerror(errno));
		return -1;
	}

	if (!yaml_parser_initialize(&parser)) {
		me_set_err(err, errsz, "yaml parser initialize failed");
		fclose(f);
		return -1;
	}
	yaml_parser_set_input_file(&parser, f);
	loaded = yaml_parser_load(&parser, &doc);
	if (!loaded) {
		me_set_err(err, errsz, "%s:%d:%d: %s", path,
		           (int)parser.problem_mark.line + 1,
		           (int)parser.problem_mark.column + 1,
		           parser.problem ? parser.problem : "yaml parse error");
		goto out;
	}
	rc = config_apply_document(cfg, &doc, path, err, errsz);
	if (rc == 0)
		rc = engine_config_validate(cfg, err, errsz);
	yaml_document_delete(&doc);
out:
	yaml_parser_delete(&parser);
	fclose(f);
	if (rc == 0)
		me_log(ME_LOG_INFO, "config loaded from %s", path);
	return rc;
}

static void print_usage(const char *prog)
{
	fprintf(stderr,
	        "Usage: %s [options]\n"
	        "Options:\n"
	        "  --config <file>  config file (default /oem/usr/share/media_engine.yaml)\n"
	        "  -a <dir>         IQ file directory, default %s\n"
	        "  -c <id>          camera id, default 0\n"
	        "  -d <device>      V4L2 device, default %s\n"
	        "  -f <format>      raw format, default %s\n"
	        "  -W <width>       width, default %d\n"
	        "  -H <height>      height, default %d\n"
	        "  -r <fps>         framerate, default %d\n"
	        "  -C <id>          KMS connector id, default %d\n"
	        "  -P <id>          KMS plane id, default %d\n"
	        "  --preview <on|off> KMS preview, default on\n"
	        "  --preview-rotation <deg> preview rotation 0/90/180/270, default 0\n"
	        "  --preview-width <px>  preview output width, default %d\n"
	        "  --preview-height <px> preview output height, default %d\n"
		"  --stream-rotation <deg> stream rotation 0/90/180/270, default 0\n"
		"  --af <mode>      focus mode: off, auto, semi-auto, manual; default off\n"
		"  -s <path>        unix socket path, default %s\n"
		"  --snapshot-dir <dir> snapshot output directory, default %s\n"
		"  --analytics-enabled <on|off> enable analytics (default off)\n"
		"  --analytics-backend <name> analytics backend (default rockiva)\n"
		"  --analytics-model <name> analytics model (required when enabled)\n"
		"  --analytics-width <px> analysis width (board-validated)\n"
		"  --analytics-height <px> analysis height (board-validated)\n"
		"  --analytics-fps <fps> analysis sampling rate (board-validated)\n"
		"  -h               show this help\n",
	        prog, ME_CFG_DEFAULT_IQ_DIR, ME_CFG_DEFAULT_DEVICE,
	        ME_CFG_DEFAULT_FORMAT, ME_CFG_DEFAULT_WIDTH, ME_CFG_DEFAULT_HEIGHT,
	        ME_CFG_DEFAULT_FPS, ME_CFG_DEFAULT_CONNECTOR, ME_CFG_DEFAULT_PLANE,
	        ME_CFG_DEFAULT_PREVIEW_WIDTH, ME_CFG_DEFAULT_PREVIEW_HEIGHT,
	        ME_CFG_DEFAULT_SOCKET, ME_CFG_DEFAULT_SNAPSHOT_DIR);
}

static const struct option long_options[] = {
	{"config", required_argument, NULL, 1000},
	{"preview", required_argument, NULL, 1001},
	{"af", required_argument, NULL, 1002},
	{"snapshot-dir", required_argument, NULL, 1003},
	{"preview-rotation", required_argument, NULL, 1004},
	{"stream-rotation", required_argument, NULL, 1005},
	{"preview-width", required_argument, NULL, 1006},
	{"preview-height", required_argument, NULL, 1007},
	{"analytics-enabled", required_argument, NULL, 1008},
	{"analytics-backend", required_argument, NULL, 1009},
	{"analytics-model", required_argument, NULL, 1010},
	{"analytics-width", required_argument, NULL, 1011},
	{"analytics-height", required_argument, NULL, 1012},
	{"analytics-fps", required_argument, NULL, 1013},
	{"help", no_argument, NULL, 'h'},
	{0, 0, 0, 0},
};

int engine_config_apply_cli(EngineConfig *cfg, int argc, char **argv,
                            char *err, size_t errsz)
{
	const char *config_file = NULL;
	int opt;
	int i;

	if (!cfg || argc < 1 || !argv) {
		me_set_err(err, errsz, "invalid command line arguments");
		return -1;
	}
	/* Locate --config before the getopt pass so the file is loaded before
	 * CLI overrides are applied. */
	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--config") && i + 1 < argc) {
			config_file = argv[i + 1];
			break;
		}
		if (!strncmp(argv[i], "--config=", 9)) {
			config_file = argv[i] + 9;
			break;
		}
	}

	if (config_file) {
		if (engine_config_load(cfg, config_file, err, errsz) != 0)
			return -1;
	}

	optind = 1;
	opterr = 0;
	while ((opt = getopt_long(argc, argv, "a:c:d:f:W:H:r:C:P:s:h",
	                          long_options, NULL)) != -1) {
		char *end = NULL;
		long v;
		bool b;

		switch (opt) {
		case 1000:
			/* --config was already loaded by the prescan above. */
			break;
		case 'a':
			snprintf(cfg->iq_dir, sizeof(cfg->iq_dir), "%s", optarg);
			break;
		case 'c':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0') {
				me_set_err(err, errsz, "invalid camera id: %s", optarg);
				return -1;
			}
			cfg->cam_id = (int)v;
			break;
		case 'd':
			snprintf(cfg->device, sizeof(cfg->device), "%s", optarg);
			break;
		case 'f':
			snprintf(cfg->format, sizeof(cfg->format), "%s", optarg);
			break;
		case 'W':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' || v <= 0) {
				me_set_err(err, errsz, "invalid width: %s", optarg);
				return -1;
			}
			cfg->width = (int)v;
			break;
		case 'H':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' || v <= 0) {
				me_set_err(err, errsz, "invalid height: %s", optarg);
				return -1;
			}
			cfg->height = (int)v;
			break;
		case 'r':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' || v <= 0) {
				me_set_err(err, errsz, "invalid fps: %s", optarg);
				return -1;
			}
			cfg->fps = (int)v;
			break;
		case 'C':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0') {
				me_set_err(err, errsz, "invalid connector id: %s", optarg);
				return -1;
			}
			cfg->connector_id = (int)v;
			break;
		case 'P':
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0') {
				me_set_err(err, errsz, "invalid plane id: %s", optarg);
				return -1;
			}
			cfg->plane_id = (int)v;
			break;
		case 's':
			snprintf(cfg->socket_path, sizeof(cfg->socket_path), "%s",
			         optarg);
			break;
		case 1001:
			if (!parse_bool_value(optarg, &b)) {
				me_set_err(err, errsz, "invalid --preview value: %s", optarg);
				return -1;
			}
			cfg->preview = b;
			break;
		case 1002:
			snprintf(cfg->af_mode, sizeof(cfg->af_mode), "%s", optarg);
			break;
		case 1003:
			snprintf(cfg->snapshot_dir, sizeof(cfg->snapshot_dir), "%s",
			         optarg);
			break;
		case 1004:
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' ||
			    (v != 0 && v != 90 && v != 180 && v != 270)) {
				me_set_err(err, errsz,
				           "invalid --preview-rotation: %s", optarg);
				return -1;
			}
			cfg->preview_rotation = (int)v;
			break;
		case 1005:
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' ||
			    (v != 0 && v != 90 && v != 180 && v != 270)) {
				me_set_err(err, errsz,
				           "invalid --stream-rotation: %s", optarg);
				return -1;
			}
			cfg->stream_rotation = (int)v;
			break;
		case 1006:
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' || v < 2 || (v & 1)) {
				me_set_err(err, errsz,
				           "invalid --preview-width: %s", optarg);
				return -1;
			}
			cfg->preview_width = (int)v;
			break;
		case 1007:
			errno = 0;
			v = strtol(optarg, &end, 10);
			if (errno || !end || *end != '\0' || v < 2 || (v & 1)) {
				me_set_err(err, errsz,
				           "invalid --preview-height: %s", optarg);
				return -1;
			}
			cfg->preview_height = (int)v;
			break;
		case 1008:
			if (!parse_bool_value(optarg, &b)) {
				me_set_err(err, errsz,
				           "invalid --analytics-enabled value: %s", optarg);
				return -1;
			}
			cfg->analytics.enabled = b;
			break;
		case 1009:
			if (set_string(cfg->analytics.backend,
			               sizeof(cfg->analytics.backend), optarg,
			               "analytics_backend", "<command line>", 0, err,
			               errsz) != 0)
				return -1;
			break;
		case 1010:
			if (set_string(cfg->analytics.model, sizeof(cfg->analytics.model),
			               optarg, "analytics_model", "<command line>", 0,
			               err, errsz) != 0)
				return -1;
			break;
		case 1011:
			if (parse_int_value(optarg, &cfg->analytics.width,
			                    "analytics_width", err, errsz) != 0)
				return -1;
			break;
		case 1012:
			if (parse_int_value(optarg, &cfg->analytics.height,
			                    "analytics_height", err, errsz) != 0)
				return -1;
			break;
		case 1013:
			if (parse_int_value(optarg, &cfg->analytics.fps,
			                    "analytics_fps", err, errsz) != 0)
				return -1;
			break;
		case 'h':
			print_usage(argv[0]);
			return 1;
		default:
			print_usage(argv[0]);
			me_set_err(err, errsz, "unknown option");
			return -1;
		}
	}
	return engine_config_validate(cfg, err, errsz);
}
