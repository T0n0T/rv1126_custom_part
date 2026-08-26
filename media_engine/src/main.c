#include "engine/engine.h"
#include "ipc/ipc_server.h"
#include "common/util.h"

#include <glib-unix.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

static gboolean on_signal(gpointer user_data)
{
	GMainLoop *loop = user_data;
	me_log(ME_LOG_INFO, "signal received, shutting down");
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void ipc_event_sink(void *userdata, const char *event,
                           const char *session_id, const char *message)
{
	IpcServer *server = userdata;
	ipc_server_broadcast_event(server, event, session_id, message);
}

static void setup_gst_environment(void)
{
	if (!getenv("GST_PLUGIN_PATH"))
		setenv("GST_PLUGIN_PATH", "/oem/usr/lib/gstreamer-1.0", 0);
	if (!getenv("GST_PLUGIN_SCANNER"))
		setenv("GST_PLUGIN_SCANNER",
		       "/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner", 0);
}

int main(int argc, char **argv)
{
	EngineConfig cfg;
	Engine engine;
	IpcServer server;
	GMainLoop *loop = NULL;
	char err[256];
	int rc;

	engine_config_defaults(&cfg);
	if (engine_config_load(&cfg, NULL, err, sizeof(err)) != 0) {
		fprintf(stderr, "media_engine: %s\n", err);
		return EXIT_FAILURE;
	}
	rc = engine_config_apply_cli(&cfg, argc, argv, err, sizeof(err));
	if (rc < 0) {
		fprintf(stderr, "media_engine: %s\n", err);
		return EXIT_FAILURE;
	}
	if (rc > 0)
		return EXIT_SUCCESS;

	setup_gst_environment();

	if (engine_init(&engine, &cfg, err, sizeof(err)) != 0) {
		fprintf(stderr, "media_engine: %s\n", err);
		return EXIT_FAILURE;
	}
	if (ipc_server_init(&server, &engine, cfg.socket_path, err, sizeof(err)) !=
	    0) {
		fprintf(stderr, "media_engine: %s\n", err);
		engine_deinit(&engine);
		return EXIT_FAILURE;
	}
	engine_set_event_sink(&engine, ipc_event_sink, &server);

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGINT, on_signal, loop);
	g_unix_signal_add(SIGTERM, on_signal, loop);
	me_log(ME_LOG_INFO, "media_engine ready (socket=%s)", cfg.socket_path);

	g_main_loop_run(loop);

	ipc_server_deinit(&server);
	engine_deinit(&engine);
	g_main_loop_unref(loop);
	me_log(ME_LOG_INFO, "media_engine exited");
	return EXIT_SUCCESS;
}
