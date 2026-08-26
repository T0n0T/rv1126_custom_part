#ifndef ME_SESSION_H
#define ME_SESSION_H

#include <stdint.h>

#define ME_SESSION_ID_MAX 128
#define ME_CHANNEL_ID_MAX 64
#define ME_DEST_IP_MAX 64
#define ME_CODEC_MAX 8

/* One live-stream session, carrying everything gst_runner needs. Field names
 * mirror the daemon protocol (see gb28181_daemon/internal/media/protocol.go). */
typedef struct {
	char session_id[ME_SESSION_ID_MAX];
	char channel_id[ME_CHANNEL_ID_MAX];
	char codec[ME_CODEC_MAX];
	int width;
	int height;
	int fps;
	int bitrate;
	char dest_ip[ME_DEST_IP_MAX];
	int dest_port;
	uint32_t ssrc;
	int payload_type;
} SessionParams;

#endif /* ME_SESSION_H */
