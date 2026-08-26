/* Host-side unit test for session_mgr with a fake LiveBackend.
 * Verifies lifecycle semantics without any GStreamer dependency.
 *
 * Build & run (host):
 *   make -C tests
 */

#include "session/session_mgr.h"
#include "common/util.h"

#include <stdio.h>
#include <string.h>

typedef struct {
	int starts;
	int stops;
	bool fail_start;
	bool fail_stop;
	char last_id[ME_SESSION_ID_MAX];
} FakeBackend;

static int fake_start(void *data, const SessionParams *p, char *err,
                      size_t errsz)
{
	FakeBackend *f = data;

	(void)p;
	if (f->fail_start) {
		me_set_err(err, errsz, "fake start failure");
		return ME_ERR_MEDIA;
	}
	f->starts++;
	return ME_ERR_OK;
}

static int fake_stop(void *data, const char *session_id, char *err,
                     size_t errsz)
{
	FakeBackend *f = data;

	if (f->fail_stop) {
		me_set_err(err, errsz, "fake stop failure");
		return ME_ERR_MEDIA;
	}
	f->stops++;
	snprintf(f->last_id, sizeof(f->last_id), "%s", session_id);
	return ME_ERR_OK;
}

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

static void fill_params(SessionParams *p, const char *id)
{
	memset(p, 0, sizeof(*p));
	snprintf(p->session_id, sizeof(p->session_id), "%s", id);
	snprintf(p->channel_id, sizeof(p->channel_id), "ch-%s", id);
	snprintf(p->codec, sizeof(p->codec), "h264");
	p->width = 1920;
	p->height = 1080;
	p->fps = 30;
	p->bitrate = 4096;
	snprintf(p->dest_ip, sizeof(p->dest_ip), "192.168.1.88");
	p->dest_port = 10003;
	p->ssrc = 123456789u;
	p->payload_type = 98;
}

int main(void)
{
	FakeBackend fake = {0};
	LiveBackend backend = {
	    .data = &fake,
	    .start_live = fake_start,
	    .stop_live = fake_stop,
	};
	SessionMgr *m;
	SessionParams p;
	char err[128];
	const MediaSession *s;

	m = session_mgr_new(&backend);
	CHECK(m != NULL, "new manager");
	CHECK(session_mgr_count(m) == 0, "initially empty");
	CHECK(!session_mgr_has(m, "s1"), "no session before start");
	CHECK(session_mgr_active(m) == NULL, "no active session when idle");

	fill_params(&p, "s1");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_OK,
	      "start first session");
	CHECK(fake.starts == 1, "backend started once");
	CHECK(session_mgr_count(m) == 1, "one session registered");
	CHECK(session_mgr_has(m, "s1"), "session present");
	s = session_mgr_active(m);
	CHECK(s != NULL && strcmp(s->params.session_id, "s1") == 0,
	      "active session is s1");
	CHECK(s != NULL && s->params.ssrc == 123456789u &&
	          s->params.payload_type == 98,
	      "session params preserved");

	fill_params(&p, "s2");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_MEDIA,
	      "second session rejected while busy");
	CHECK(fake.starts == 1, "backend not started for rejected session");
	CHECK(session_mgr_count(m) == 1, "still one session");

	fill_params(&p, "s1");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_MEDIA,
	      "duplicate session id rejected");

	CHECK(session_mgr_stop(m, "nope", err, sizeof(err)) == ME_ERR_NOT_FOUND,
	      "stop unknown session -> not found");
	CHECK(session_mgr_stop(m, "s1", err, sizeof(err)) == ME_ERR_OK,
	      "stop s1");
	CHECK(fake.stops == 1 && strcmp(fake.last_id, "s1") == 0,
	      "backend stopped with correct id");
	CHECK(session_mgr_count(m) == 0, "table empty after stop");
	CHECK(!session_mgr_has(m, "s1"), "session removed after stop");

	fake.fail_start = true;
	fill_params(&p, "s3");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_MEDIA,
	      "backend start failure propagates");
	CHECK(session_mgr_count(m) == 0, "no session registered on start failure");
	fake.fail_start = false;

	fill_params(&p, "s4");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_OK,
	      "start s4 after failure");
	fake.fail_stop = true;
	CHECK(session_mgr_stop(m, "s4", err, sizeof(err)) == ME_ERR_MEDIA,
	      "backend stop failure propagates");
	CHECK(session_mgr_count(m) == 0, "session removed even when stop failed");
	fake.fail_stop = false;

	/* Shutdown must stop active sessions. */
	fill_params(&p, "s5");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_OK,
	      "start s5 for shutdown");
	CHECK(session_mgr_stop(m, "s5", err, sizeof(err)) == ME_ERR_OK,
	      "stop s5 before free");
	session_mgr_free(m);
	m = NULL;
	CHECK(1, "free with no active sessions");

	m = session_mgr_new(&backend);
	fill_params(&p, "s6");
	CHECK(session_mgr_start(m, &p, err, sizeof(err)) == ME_ERR_OK,
	      "start s6 for shutdown-stop");
	{
		int stops_before = fake.stops;
		session_mgr_free(m);
		CHECK(fake.stops == stops_before + 1 &&
		          strcmp(fake.last_id, "s6") == 0,
		      "free stops remaining session");
	}

	CHECK(session_mgr_new(NULL) == NULL, "null backend rejected");

	if (failures == 0)
		printf("\nsession_mgr: all tests passed\n");
	else
		printf("\nsession_mgr: %d test(s) failed\n", failures);
	return failures == 0 ? 0 : 1;
}
