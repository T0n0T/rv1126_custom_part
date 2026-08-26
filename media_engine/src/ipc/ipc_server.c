#include "ipc/ipc_server.h"

#include "cJSON.h"
#include "common/util.h"

#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define ME_IPC_MAX_LINE (64 * 1024)
#define ME_IPC_BACKLOG 8
#define ME_IPC_READ_CHUNK 8192
#define ME_IPC_ERR_PARSE (-32600)

typedef struct IpcClientSource {
	GSource base;
	GPollFD pfd;
	IpcServer *server;
	IpcClient *client;
} IpcClientSource;

struct IpcClient {
	int fd;
	GByteArray *in;
	GQueue *out; /* pending strings, written when G_IO_OUT fires */
	bool alive;
	IpcClientSource *source;
};

/* ---------- forward declarations ---------- */

static void ipc_client_close(IpcServer *s, IpcClient *c);
static bool ipc_client_io(IpcServer *s, IpcClient *c, GIOCondition revents);
static bool ipc_client_send(IpcServer *s, IpcClient *c, const char *text);
static bool ipc_handle_line(IpcServer *s, IpcClient *c, const char *line,
                            size_t len);
static void ipc_send_error(IpcServer *s, IpcClient *c, uint64_t id, int code,
                           const char *message);

/* ---------- custom GSource for client sockets ---------- */

static gboolean ipc_client_source_prepare(GSource *src, gint *timeout)
{
	(void)src;
	*timeout = -1;
	return FALSE;
}

static gboolean ipc_client_source_check(GSource *src)
{
	IpcClientSource *cs = (IpcClientSource *)src;
	return !!(cs->pfd.revents &
	          (G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL));
}

static gboolean ipc_client_source_dispatch(GSource *src, GSourceFunc callback,
                                           gpointer user_data)
{
	IpcClientSource *cs = (IpcClientSource *)src;
	(void)callback;
	(void)user_data;
	return ipc_client_io(cs->server, cs->client, cs->pfd.revents)
	           ? G_SOURCE_CONTINUE
	           : G_SOURCE_REMOVE;
}

static void ipc_client_source_finalize(GSource *src)
{
	(void)src;
}

/* GLib only dispatches sources that have a callback installed; the real work
 * happens in ipc_client_source_dispatch, this is just the required non-NULL
 * callback. */
static gboolean ipc_client_source_callback(gpointer user_data)
{
	(void)user_data;
	return G_SOURCE_CONTINUE;
}

static GSourceFuncs ipc_client_source_funcs = {
	.prepare = ipc_client_source_prepare,
	.check = ipc_client_source_check,
	.dispatch = ipc_client_source_dispatch,
	.finalize = ipc_client_source_finalize,
};

/* ---------- client lifecycle and I/O ---------- */

/* Re-arms the poll events. Must only be called while the client is alive. */
static void ipc_client_arm_output(IpcClient *c)
{
	IpcClientSource *cs = c->source;
	GIOCondition want = G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;

	if (!g_queue_is_empty(c->out))
		want |= G_IO_OUT;
	if (cs->pfd.events != want) {
		cs->pfd.events = want;
		g_main_context_wakeup(NULL);
	}
}

/* Writes as much as possible; returns bytes consumed (len = all done),
 * < len when the socket buffer is full, -1 on fatal error. */
static ssize_t ipc_try_write(IpcClient *c, const char *data, size_t len)
{
	size_t off = 0;

	while (off < len) {
		ssize_t n = write(c->fd, data + off, len - off);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			break;
		if (n < 0)
			return -1;
		break; /* n == 0 */
	}
	return (ssize_t)off;
}

static bool ipc_client_send(IpcServer *s, IpcClient *c, const char *text)
{
	size_t len = strlen(text);

	if (g_queue_is_empty(c->out)) {
		ssize_t n = ipc_try_write(c, text, len);
		if (n < 0) {
			me_log(ME_LOG_WARN, "ipc: write failed fd=%d: %s", c->fd,
			       strerror(errno));
			ipc_client_close(s, c);
			return false;
		}
		if ((size_t)n == len)
			return true;
		text += n;
		len -= n;
	}
	g_queue_push_tail(c->out, g_strndup(text, len));
	ipc_client_arm_output(c);
	return true;
}

static bool ipc_client_flush(IpcServer *s, IpcClient *c)
{
	while (!g_queue_is_empty(c->out)) {
		gchar *head = g_queue_pop_head(c->out);
		size_t len = strlen(head);
		ssize_t n = ipc_try_write(c, head, len);

		if (n < 0) {
			g_free(head);
			me_log(ME_LOG_WARN, "ipc: flush failed fd=%d: %s", c->fd,
			       strerror(errno));
			ipc_client_close(s, c);
			return false;
		}
		if ((size_t)n < len) {
			g_queue_push_head(c->out, g_strdup(head + n));
			g_free(head);
			break;
		}
		g_free(head);
	}
	ipc_client_arm_output(c);
	return true;
}

static void ipc_client_close(IpcServer *s, IpcClient *c)
{
	if (!c->alive)
		return;
	c->alive = false;
	s->clients = g_slist_remove(s->clients, c);
	close(c->fd);
	g_byte_array_free(c->in, TRUE);
	while (!g_queue_is_empty(c->out))
		g_free(g_queue_pop_head(c->out));
	g_queue_free(c->out);
	g_source_remove_poll(&c->source->base, &c->source->pfd);
	g_source_destroy(&c->source->base);
	me_log(ME_LOG_DEBUG, "ipc: client closed fd=%d", c->fd);
	g_free(c);
}

static void ipc_client_create(IpcServer *s, int fd)
{
	IpcClient *c;
	IpcClientSource *cs;
	int flags;

	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);

	c = g_new0(IpcClient, 1);
	c->fd = fd;
	c->in = g_byte_array_new();
	c->out = g_queue_new();
	c->alive = true;

	cs = (IpcClientSource *)g_source_new(&ipc_client_source_funcs,
	                                     sizeof(IpcClientSource));
	cs->server = s;
	cs->client = c;
	cs->pfd.fd = fd;
	cs->pfd.events = G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL;
	cs->pfd.revents = 0;
	c->source = cs;

	g_source_set_callback(&cs->base, ipc_client_source_callback, c, NULL);
	g_source_add_poll(&cs->base, &cs->pfd);
	g_source_attach(&cs->base, NULL);
	g_source_unref(&cs->base); /* the context keeps its own reference */
	s->clients = g_slist_prepend(s->clients, c);
	me_log(ME_LOG_DEBUG, "ipc: client connected fd=%d", fd);
}

static bool ipc_client_process_input(IpcServer *s, IpcClient *c)
{
	for (;;) {
		guint8 *nl = memchr(c->in->data, '\n', c->in->len);
		if (!nl) {
			if (c->in->len > ME_IPC_MAX_LINE) {
				me_log(ME_LOG_WARN,
				       "ipc: line exceeds %d bytes, discarding buffer",
				       ME_IPC_MAX_LINE);
				ipc_send_error(s, c, 0, ME_IPC_ERR_PARSE,
				               "message too long");
				g_byte_array_remove_range(c->in, 0, c->in->len);
			}
			return c->alive;
		}
		{
			size_t pos = (size_t)(nl - c->in->data);
			if (!ipc_handle_line(s, c, (const char *)c->in->data, pos))
				return false;
			g_byte_array_remove_range(c->in, 0, pos + 1);
		}
	}
}

static bool ipc_client_io(IpcServer *s, IpcClient *c, GIOCondition revents)
{
	if (revents & (G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
		char buf[ME_IPC_READ_CHUNK];
		for (;;) {
			ssize_t n = read(c->fd, buf, sizeof(buf));
			if (n > 0) {
				g_byte_array_append(c->in, (const guint8 *)buf, (guint)n);
				if (!ipc_client_process_input(s, c)) {
					ipc_client_close(s, c);
					return false;
				}
				if (n < (ssize_t)sizeof(buf))
					break;
				continue;
			}
			if (n == 0) {
				ipc_client_close(s, c);
				return false;
			}
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			me_log(ME_LOG_WARN, "ipc: read error fd=%d: %s", c->fd,
			       strerror(errno));
			ipc_client_close(s, c);
			return false;
		}
	}
	if (!c->alive)
		return false;
	if (revents & G_IO_OUT) {
		if (!ipc_client_flush(s, c))
			return false;
	}
	return c->alive;
}

/* ---------- request/response handling ---------- */

static const char *json_string(cJSON *params, const char *key)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
	if (!cJSON_IsString(v))
		return NULL;
	return v->valuestring;
}

static bool json_int(cJSON *params, const char *key, int *out)
{
	cJSON *v = cJSON_GetObjectItemCaseSensitive(params, key);
	double d;

	if (!cJSON_IsNumber(v))
		return false;
	d = v->valuedouble;
	if (d != floor(d) || d < -2147483648.0 || d > 2147483647.0)
		return false;
	*out = (int)d;
	return true;
}

static void ipc_send_result(IpcServer *s, IpcClient *c, uint64_t id,
                            cJSON *result)
{
	cJSON *resp = cJSON_CreateObject();
	char *text;
	gchar *line;

	if (!result)
		result = cJSON_CreateObject();
	cJSON_AddNumberToObject(resp, "v", 1);
	cJSON_AddNumberToObject(resp, "id", (double)id);
	cJSON_AddItemToObject(resp, "result", result);
	text = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!text)
		return;
	line = g_strdup_printf("%s\n", text);
	cJSON_free(text);
	ipc_client_send(s, c, line);
	g_free(line);
}

static void ipc_send_error(IpcServer *s, IpcClient *c, uint64_t id, int code,
                           const char *message)
{
	cJSON *err = cJSON_CreateObject();
	cJSON *resp = cJSON_CreateObject();
	char *text;
	gchar *line;

	cJSON_AddNumberToObject(err, "code", code);
	cJSON_AddStringToObject(err, "message", message ? message : "error");
	cJSON_AddNumberToObject(resp, "v", 1);
	cJSON_AddNumberToObject(resp, "id", (double)id);
	cJSON_AddItemToObject(resp, "error", err);
	text = cJSON_PrintUnformatted(resp);
	cJSON_Delete(resp);
	if (!text)
		return;
	line = g_strdup_printf("%s\n", text);
	cJSON_free(text);
	ipc_client_send(s, c, line);
	g_free(line);
}

static bool ipc_bad_params(IpcServer *s, IpcClient *c, uint64_t id,
                           const char *message)
{
	ipc_send_error(s, c, id, -32002, message);
	return c->alive;
}

static bool ipc_handle_start_live(IpcServer *s, IpcClient *c, uint64_t id,
                                  cJSON *params)
{
	SessionParams p;
	const char *sval;
	char errbuf[256];
	int rc;

	memset(&p, 0, sizeof(p));
	if (!cJSON_IsObject(params))
		return ipc_bad_params(s, c, id,
		                       "start_live: params must be an object");

	sval = json_string(params, "session_id");
	if (!sval || !*sval)
		return ipc_bad_params(s, c, id,
		                       "start_live: session_id required");
	snprintf(p.session_id, sizeof(p.session_id), "%s", sval);

	sval = json_string(params, "channel_id");
	if (!sval || !*sval)
		return ipc_bad_params(s, c, id,
		                       "start_live: channel_id required");
	snprintf(p.channel_id, sizeof(p.channel_id), "%s", sval);

	sval = json_string(params, "codec");
	if (!sval || !*sval)
		return ipc_bad_params(s, c, id, "start_live: codec required");
	snprintf(p.codec, sizeof(p.codec), "%s", sval);

	if (!json_int(params, "width", &p.width) || p.width <= 0)
		return ipc_bad_params(s, c, id,
		                       "start_live: width must be positive integer");
	if (!json_int(params, "height", &p.height) || p.height <= 0)
		return ipc_bad_params(s, c, id,
		                       "start_live: height must be positive integer");
	if (!json_int(params, "fps", &p.fps) || p.fps <= 0)
		return ipc_bad_params(s, c, id,
		                       "start_live: fps must be positive integer");
	if (!json_int(params, "bitrate", &p.bitrate) || p.bitrate <= 0)
		return ipc_bad_params(s, c, id,
		                       "start_live: bitrate must be positive integer");
	if (!json_int(params, "dest_port", &p.dest_port) || p.dest_port < 1 ||
	    p.dest_port > 65535)
		return ipc_bad_params(s, c, id,
		                       "start_live: dest_port out of range");
	if (!json_int(params, "payload_type", &p.payload_type) ||
	    p.payload_type < 0 || p.payload_type > 127)
		return ipc_bad_params(s, c, id,
		                       "start_live: payload_type must be 0..127");

	sval = json_string(params, "dest_ip");
	if (!sval || !me_valid_ipv4(sval))
		return ipc_bad_params(s, c, id,
		                       "start_live: dest_ip invalid IPv4");
	snprintf(p.dest_ip, sizeof(p.dest_ip), "%s", sval);

	sval = json_string(params, "ssrc");
	if (!sval || !me_parse_u32_decimal(sval, &p.ssrc))
		return ipc_bad_params(
		    s, c, id, "start_live: ssrc must be decimal uint32");

	rc = engine_start_live(s->engine, &p, errbuf, sizeof(errbuf));
	if (rc != ME_ERR_OK) {
		ipc_send_error(s, c, id, rc, errbuf);
		return c->alive;
	}
	{
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "ok", TRUE);
		ipc_send_result(s, c, id, result);
	}
	return c->alive;
}

static bool ipc_handle_stop_live(IpcServer *s, IpcClient *c, uint64_t id,
                                 cJSON *params)
{
	const char *sval;
	char errbuf[256];
	int rc;

	if (!cJSON_IsObject(params))
		return ipc_bad_params(s, c, id,
		                       "stop_live: params must be an object");
	sval = json_string(params, "session_id");
	if (!sval || !*sval)
		return ipc_bad_params(s, c, id, "stop_live: session_id required");

	rc = engine_stop_live(s->engine, sval, errbuf, sizeof(errbuf));
	if (rc != ME_ERR_OK) {
		ipc_send_error(s, c, id, rc, errbuf);
		return c->alive;
	}
	{
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "ok", TRUE);
		ipc_send_result(s, c, id, result);
	}
	return c->alive;
}

static bool ipc_handle_snapshot(IpcServer *s, IpcClient *c, uint64_t id,
                                cJSON *params)
{
	const char *sval;
	char errbuf[256];
	int rc;

	if (!cJSON_IsObject(params))
		return ipc_bad_params(s, c, id,
		                       "snapshot: params must be an object");
	sval = json_string(params, "channel_id");
	if (!sval || !*sval)
		return ipc_bad_params(s, c, id, "snapshot: channel_id required");

	rc = engine_snapshot(s->engine, sval, errbuf, sizeof(errbuf));
	if (rc != ME_ERR_OK) {
		ipc_send_error(s, c, id, rc, errbuf);
		return c->alive;
	}
	{
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "ok", TRUE);
		ipc_send_result(s, c, id, result);
	}
	return c->alive;
}

static bool ipc_handle_get_status(IpcServer *s, IpcClient *c, uint64_t id)
{
	EngineStatus st;
	cJSON *result;

	engine_get_status(s->engine, &st);
	result = cJSON_CreateObject();
	cJSON_AddBoolToObject(result, "running", st.running);
	cJSON_AddNumberToObject(result, "fps", st.fps);
	cJSON_AddNumberToObject(result, "bitrate", st.bitrate);
	ipc_send_result(s, c, id, result);
	return c->alive;
}

static bool ipc_handle_line(IpcServer *s, IpcClient *c, const char *line,
                            size_t len)
{
	cJSON *root = NULL;
	cJSON *v = NULL;
	cJSON *idj = NULL;
	cJSON *method = NULL;
	cJSON *params = NULL;
	uint64_t id = 0;
	const char *method_name;

	while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
		len--;
	if (len == 0)
		return true;

	root = cJSON_ParseWithLength(line, len);
	if (!root || !cJSON_IsObject(root)) {
		ipc_send_error(s, c, 0, ME_IPC_ERR_PARSE,
		               "invalid JSON: object expected");
		goto out;
	}
	v = cJSON_GetObjectItemCaseSensitive(root, "v");
	if (!cJSON_IsNumber(v) || v->valuedouble != 1.0) {
		ipc_send_error(s, c, 0, ME_IPC_ERR_PARSE,
		               "unsupported protocol version");
		goto out;
	}
	idj = cJSON_GetObjectItemCaseSensitive(root, "id");
	if (!cJSON_IsNumber(idj) || idj->valuedouble < 0 ||
	    idj->valuedouble != floor(idj->valuedouble)) {
		ipc_send_error(s, c, 0, ME_IPC_ERR_PARSE, "invalid id");
		goto out;
	}
	id = (uint64_t)idj->valuedouble;

	method = cJSON_GetObjectItemCaseSensitive(root, "method");
	if (!cJSON_IsString(method) || !method->valuestring) {
		ipc_send_error(s, c, id, ME_IPC_ERR_PARSE, "method required");
		goto out;
	}
	method_name = method->valuestring;
	params = cJSON_GetObjectItemCaseSensitive(root, "params");

	if (!strcmp(method_name, "media.ping")) {
		cJSON *result = cJSON_CreateObject();
		cJSON_AddBoolToObject(result, "ok", TRUE);
		ipc_send_result(s, c, id, result);
	} else if (!strcmp(method_name, "media.start_live")) {
		if (!ipc_handle_start_live(s, c, id, params))
			goto out;
	} else if (!strcmp(method_name, "media.stop_live")) {
		if (!ipc_handle_stop_live(s, c, id, params))
			goto out;
	} else if (!strcmp(method_name, "media.snapshot")) {
		if (!ipc_handle_snapshot(s, c, id, params))
			goto out;
	} else if (!strcmp(method_name, "media.get_status")) {
		if (!ipc_handle_get_status(s, c, id))
			goto out;
	} else {
		char msg[128];
		snprintf(msg, sizeof(msg), "unknown method: %s", method_name);
		ipc_send_error(s, c, id, ME_IPC_ERR_PARSE, msg);
	}

out:
	if (root)
		cJSON_Delete(root);
	return c->alive;
}

/* ---------- listener ---------- */

static gboolean ipc_server_accept_cb(gint fd, GIOCondition condition,
                                     gpointer user_data)
{
	IpcServer *s = user_data;
	(void)condition;

	for (;;) {
		int cfd = accept(fd, NULL, NULL);
		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;
			me_log(ME_LOG_ERROR, "ipc: accept failed: %s", strerror(errno));
			break;
		}
		ipc_client_create(s, cfd);
	}
	return G_SOURCE_CONTINUE;
}

int ipc_server_init(IpcServer *s, Engine *engine, const char *path, char *err,
                    size_t errsz)
{
	struct sockaddr_un addr;
	int probe;
	int fd;

	memset(s, 0, sizeof(*s));
	s->engine = engine;
	s->listen_fd = -1;
	snprintf(s->socket_path, sizeof(s->socket_path), "%s", path);

	if (strlen(path) >= sizeof(addr.sun_path)) {
		me_set_err(err, errsz, "unix socket path too long: %s", path);
		return -1;
	}

	/* Refuse to take over a socket owned by a live media_engine. */
	probe = socket(AF_UNIX, SOCK_STREAM, 0);
	if (probe < 0) {
		me_set_err(err, errsz, "socket(AF_UNIX): %s", strerror(errno));
		return -1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(probe, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		close(probe);
		me_set_err(err, errsz, "another media_engine is listening on %s",
		           path);
		return -1;
	}
	close(probe);
	unlink(path);

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		me_set_err(err, errsz, "socket(AF_UNIX): %s", strerror(errno));
		return -1;
	}
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		me_set_err(err, errsz, "bind %s: %s", path, strerror(errno));
		close(fd);
		return -1;
	}
	chmod(path, 0600);
	if (listen(fd, ME_IPC_BACKLOG) != 0) {
		me_set_err(err, errsz, "listen %s: %s", path, strerror(errno));
		close(fd);
		unlink(path);
		return -1;
	}
	{
		int flags = fcntl(fd, F_GETFL, 0);
		if (flags >= 0)
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	}
	s->listen_fd = fd;
	s->listen_source = g_unix_fd_add(fd, G_IO_IN, ipc_server_accept_cb, s);
	return 0;
}

void ipc_server_deinit(IpcServer *s)
{
	if (!s)
		return;
	if (s->listen_source) {
		g_source_remove(s->listen_source);
		s->listen_source = 0;
	}
	if (s->listen_fd >= 0) {
		close(s->listen_fd);
		s->listen_fd = -1;
	}
	while (s->clients)
		ipc_client_close(s, (IpcClient *)s->clients->data);
	s->clients = NULL;
	unlink(s->socket_path);
}

void ipc_server_broadcast_event(IpcServer *s, const char *event,
                                const char *session_id, const char *message)
{
	cJSON *params = cJSON_CreateObject();
	cJSON *notif = cJSON_CreateObject();
	char *text;
	size_t len;
	gchar *line;
	GSList *it;

	if (!s || !event)
		return;
	cJSON_AddStringToObject(params, "event", event);
	if (session_id)
		cJSON_AddStringToObject(params, "session_id", session_id);
	if (message)
		cJSON_AddStringToObject(params, "message", message);
	cJSON_AddNumberToObject(notif, "v", 1);
	cJSON_AddStringToObject(notif, "method", "media.event");
	cJSON_AddItemToObject(notif, "params", params);

	text = cJSON_PrintUnformatted(notif);
	cJSON_Delete(notif);
	if (!text)
		return;
	len = strlen(text);
	line = g_malloc(len + 2);
	memcpy(line, text, len);
	line[len] = '\n';
	line[len + 1] = '\0';
	cJSON_free(text);

	for (it = s->clients; it; it = it->next) {
		IpcClient *c = it->data;
		if (!ipc_client_send(s, c, line))
			me_log(ME_LOG_WARN, "ipc: event dropped for fd=%d", c->fd);
	}
	g_free(line);
}
