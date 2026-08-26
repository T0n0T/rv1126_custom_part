/* Board-side RPC smoke client for media_engine.
 * Test tool only, NOT part of the product build.
 *
 * Usage: rpc_client <unix-socket> <method> [params-json] [id]
 * Example:
 *   rpc_client /tmp/me.sock media.ping
 *   rpc_client /tmp/me.sock media.start_live \
 *     '{"session_id":"s1","channel_id":"ch1","codec":"h264","width":1920,"height":1080,"fps":30,"bitrate":4096,"dest_ip":"192.168.1.88","dest_port":20000,"ssrc":"123456789","payload_type":98}'
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int write_all(int fd, const char *data, size_t len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return -1;
		data += n;
		len -= (size_t)n;
	}
	return 0;
}

int main(int argc, char **argv)
{
	const char *path;
	const char *method;
	const char *params;
	unsigned long long id;
	char req[65536];
	char buf[65536];
	struct sockaddr_un addr;
	ssize_t total = 0;
	int fd;
	int n;

	if (argc < 3) {
		fprintf(stderr,
		        "usage: %s <socket> <method> [params-json] [id]\n",
		        argv[0]);
		return 2;
	}
	path = argv[1];
	method = argv[2];
	params = argc > 3 ? argv[3] : NULL;
	id = argc > 4 ? strtoull(argv[4], NULL, 10) : 1;

	n = snprintf(req, sizeof(req), "{\"v\":1,\"id\":%llu,\"method\":\"%s\"%s%s}",
	             id, method, params ? ",\"params\":" : "",
	             params ? params : "");
	if (n <= 0 || n >= (int)sizeof(req)) {
		fprintf(stderr, "request too long\n");
		return 2;
	}

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		perror("socket");
		return 1;
	}
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	if (strlen(path) >= sizeof(addr.sun_path)) {
		fprintf(stderr, "socket path too long\n");
		return 2;
	}
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		perror("connect");
		return 1;
	}
	if (write_all(fd, req, (size_t)n) != 0 ||
	    write_all(fd, "\n", 1) != 0) {
		perror("write");
		return 1;
	}
	while (total < (ssize_t)sizeof(buf) - 1) {
		ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total);
		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			break;
		total += r;
		if (memchr(buf, '\n', (size_t)total))
			break;
	}
	if (total > 0) {
		char *nl;
		buf[total] = '\0';
		nl = strchr(buf, '\n');
		if (nl)
			*nl = '\0';
		printf("%s\n", buf);
	}
	close(fd);
	return 0;
}
