/* Host-side unit test for the YAML-subset config parser.
 * Build & run (host): make -C tests
 */

#include "config/config.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond, name)                                                     \
	do {                                                                      \
		if (cond) {                                                           \
			printf("[PASS] %s\n", name);                                      \
		} else {                                                              \
			printf("[FAIL] %s (%s:%d)\n", name, __FILE__, __LINE__);          \
			failures++;                                                       \
		}                                                                     \
	} while (0)

static int load_content(const char *content, EngineConfig *cfg, char *err,
                        size_t errsz)
{
	char tmpl[] = "/tmp/me_config_test_XXXXXX";
	int fd;
	int rc;

	fd = mkstemp(tmpl);
	if (fd < 0)
		return -2;
	if (write(fd, content, strlen(content)) != (ssize_t)strlen(content)) {
		close(fd);
		unlink(tmpl);
		return -2;
	}
	close(fd);
	engine_config_defaults(cfg);
	rc = engine_config_load(cfg, tmpl, err, errsz);
	unlink(tmpl);
	return rc;
}

int main(void)
{
	EngineConfig cfg;
	char err[256];
	int rc;

	/* 1. Full valid YAML. */
	rc = load_content(
	    "# comment line\n"
	    "cam_id: 1\n"
	    "iq_dir: /oem/iq\n"
	    "device: /dev/video24\n"
	    "format: NV12\n"
	    "width: 1280\n"
	    "height: 720\n"
	    "fps: 25\n"
	    "connector_id: 12\n"
	    "plane_id: 34\n"
	    "preview: off\n"
	    "af_mode: auto\n"
	    "socket: /tmp/me.sock\n"
	    "snapshot_dir: /data/snaps\n",
	    &cfg, err, sizeof(err));
	CHECK(rc == 0, "valid yaml loads");
	CHECK(cfg.cam_id == 1 && cfg.width == 1280 && cfg.height == 720 &&
	          cfg.fps == 25 && cfg.connector_id == 12 && cfg.plane_id == 34,
	      "numeric fields parsed");
	CHECK(cfg.preview == false, "preview: off parsed as false");
	CHECK(strcmp(cfg.af_mode, "auto") == 0, "af_mode parsed");
	CHECK(strcmp(cfg.socket_path, "/tmp/me.sock") == 0, "socket parsed");
	CHECK(strcmp(cfg.snapshot_dir, "/data/snaps") == 0,
	      "snapshot_dir parsed");

	/* 1b. Blank lines are tolerated. */
	rc = load_content("width: 640\n\n\nfps: 25\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.width == 640 && cfg.fps == 25,
	      "blank lines tolerated");

	/* 2. Quotes protect '#', inline comments are stripped outside quotes. */
	rc = load_content(
	    "device: \"/dev/video24\"  # node\n"
	    "snapshot_dir: '/data/snaps # keep'\n"
	    "af_mode: \"manual\"\n",
	    &cfg, err, sizeof(err));
	CHECK(rc == 0, "quoted values load");
	CHECK(strcmp(cfg.device, "/dev/video24") == 0, "double quotes stripped");
	CHECK(strcmp(cfg.snapshot_dir, "/data/snaps # keep") == 0,
	      "single quotes keep '#'");
	CHECK(strcmp(cfg.af_mode, "manual") == 0, "quoted af_mode");

	/* 3. Boolean spellings. */
	rc = load_content("preview: yes\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.preview == true, "preview: yes");
	rc = load_content("preview: true\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.preview == true, "preview: true");
	rc = load_content("preview: 1\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.preview == true, "preview: 1");
	rc = load_content("preview: no\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.preview == false, "preview: no");

	/* 4. Unknown keys are ignored with a warning. */
	rc = load_content("width: 640\nnot_a_key: 42\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.width == 640, "unknown key ignored");

	/* 5. A plain scalar document ("width = 640") is not a mapping. */
	rc = load_content("width = 640\n", &cfg, err, sizeof(err));
	CHECK(rc != 0, "non-mapping root rejected");

	/* 6. Root-level indentation is valid YAML; sequences are not a mapping. */
	rc = load_content("  width: 640\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.width == 640, "indented top-level key accepted");
	rc = load_content("- width: 640\n", &cfg, err, sizeof(err));
	CHECK(rc != 0, "sequence root rejected");
	rc = load_content("width: {x: 1}\n", &cfg, err, sizeof(err));
	CHECK(rc != 0, "known key with mapping value rejected");
	rc = load_content("width 640\n", &cfg, err, sizeof(err));
	CHECK(rc != 0, "malformed mapping rejected");
	rc = load_content("width:\n", &cfg, err, sizeof(err));
	CHECK(rc != 0, "empty value rejected");

	/* 7. Anchors/aliases and block scalars resolve through libyaml. */
	rc = load_content("defaults: &d 640\nwidth: *d\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.width == 640, "alias value resolved");
	rc = load_content("fps: |\n  25\n", &cfg, err, sizeof(err));
	CHECK(rc == 0 && cfg.fps == 25, "block scalar value parsed");

	/* 8. Missing file is not an error (defaults stay). */
	engine_config_defaults(&cfg);
	rc = engine_config_load(&cfg, "/tmp/me_config_test_does_not_exist.yaml",
	                        err, sizeof(err));
	CHECK(rc == 0 && cfg.width == ME_CFG_DEFAULT_WIDTH,
	      "missing file falls back to defaults");

	if (failures == 0)
		printf("\nconfig: all tests passed\n");
	else
		printf("\nconfig: %d test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
