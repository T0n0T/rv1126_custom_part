#ifndef ME_IPC_SERVER_H
#define ME_IPC_SERVER_H

#include <glib.h>
#include <stdbool.h>
#include <stddef.h>

#include "engine/engine.h"

typedef struct IpcClient IpcClient;

typedef struct {
	Engine *engine;
	char socket_path[128];
	int listen_fd;
	guint listen_source;
	GSList *clients;
} IpcServer;

int ipc_server_init(IpcServer *s, Engine *engine, const char *path, char *err,
                    size_t errsz);
void ipc_server_deinit(IpcServer *s);

/* Best-effort event notification to all connected daemon clients:
 * {"v":1,"method":"media.event","params":{event, session_id?, message?}}.
 * May be dropped when no daemon connection is open; never blocks. */
void ipc_server_broadcast_event(IpcServer *s, const char *event,
                                const char *session_id, const char *message);

#endif /* ME_IPC_SERVER_H */
