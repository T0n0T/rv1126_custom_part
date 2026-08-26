#ifndef ME_CONFIG_H
#define ME_CONFIG_H

#include <stdbool.h>
#include <stddef.h>

#define ME_CFG_PATH_MAX 256
#define ME_CFG_SOCKET_MAX 128

#define ME_CFG_DEFAULT_SOCKET "/var/run/gb28181_media.sock"
#define ME_CFG_DEFAULT_IQ_DIR "/oem/usr/share/iqfiles"
#define ME_CFG_DEFAULT_DEVICE "/dev/video24"
#define ME_CFG_DEFAULT_FORMAT "NV12"
#define ME_CFG_DEFAULT_WIDTH 3840
#define ME_CFG_DEFAULT_HEIGHT 2160
#define ME_CFG_DEFAULT_FPS 30
#define ME_CFG_DEFAULT_CONNECTOR 97
#define ME_CFG_DEFAULT_PLANE 75
#define ME_CFG_DEFAULT_PREVIEW_WIDTH 480
#define ME_CFG_DEFAULT_PREVIEW_HEIGHT 800
#define ME_CFG_DEFAULT_SNAPSHOT_DIR "/data/media_engine/snapshots"

typedef struct {
	int cam_id;
	char iq_dir[ME_CFG_PATH_MAX];
	char device[ME_CFG_SOCKET_MAX];
	char format[16];
	int width;
	int height;
	int fps;
	int connector_id;
	int plane_id;
	bool preview;
	int preview_rotation; /* 0/90/180/270, applied to the KMS preview only */
	int preview_width;    /* KMS preview output width (RGA scale) */
	int preview_height;   /* KMS preview output height (RGA scale) */
	int stream_rotation;  /* 0/90/180/270, applied to the live branch only */
	char af_mode[16];
	char socket_path[ME_CFG_SOCKET_MAX];
	char snapshot_dir[ME_CFG_PATH_MAX];
} EngineConfig;

void engine_config_defaults(EngineConfig *cfg);

/* Loads a YAML config file via libyaml (static, third_party/libyaml). The
 * schema is a top-level mapping whose known keys have scalar values; unknown
 * keys are ignored with a warning, non-scalar values for known keys are
 * rejected. When path is NULL the default /oem/usr/share/media_engine.yaml is
 * used; a missing file is not an error. Returns 0 on success, -1 with err
 * filled on parse failure. */
int engine_config_load(EngineConfig *cfg, const char *path, char *err,
                       size_t errsz);

/* Applies command line overrides on top of cfg. If --config <path> is given,
 * that file is loaded first. Returns 0 on success, 1 when help was shown,
 * -1 on error (err filled). */
int engine_config_apply_cli(EngineConfig *cfg, int argc, char **argv,
                            char *err, size_t errsz);

#endif /* ME_CONFIG_H */
