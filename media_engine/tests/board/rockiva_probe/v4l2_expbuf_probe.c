/*
 * V4L2 multi-planar DMA-BUF exporter capability probe (test tool only).
 *
 * This deliberately stops at the kernel V4L2 queue/export boundary.  It does
 * not use GStreamer, RockIVA, the production media_engine, or any inference
 * path.  A successful run proves only that the selected V4L2 node accepted
 * the requested format, allocated MMAP buffers, and exported valid fds.
 */

#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "mainpath_guard.h"

#define V4L2_CAPTURE_TYPE V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
#define V4L2_MEMORY_TYPE V4L2_MEMORY_MMAP
#define MAX_REQUESTED_BUFFERS 64U

struct probe_options {
	const char *device;
	unsigned int width;
	unsigned int height;
	unsigned int frames;
	int allow_mainpath;
};

struct queried_buffer {
	struct v4l2_plane planes[VIDEO_MAX_PLANES];
	unsigned int plane_count;
};

static int ioctl_checked(int fd, unsigned long request, void *arg,
				 const char *name)
{
	int result;

	do {
		result = ioctl(fd, request, arg);
	} while (result < 0 && errno == EINTR);
	if (result < 0) {
		int saved_errno = errno;

		fprintf(stderr, "%s failed: errno=%d (%s)\n", name, saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	return 0;
}

static int close_checked(int *fd, const char *name)
{
	int result;
	int saved_errno;

	if (!fd || *fd < 0)
		return 0;
	result = close(*fd);
	saved_errno = errno;
	*fd = -1;
	if (result < 0) {
		fprintf(stderr, "%s failed: errno=%d (%s)\n", name, saved_errno,
			strerror(saved_errno));
		return -1;
	}
	return 0;
}

static int parse_positive_uint(const char *value, unsigned int maximum,
				       unsigned int *out)
{
	char *end = NULL;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(value, &end, 0);
	if (errno != 0 || end == value || *end != '\0' || parsed == 0 ||
	    parsed > maximum)
		return -1;
	*out = (unsigned int)parsed;
	return 0;
}

static void usage(const char *program)
{
	fprintf(stderr, "usage: %s --device PATH --width N --height N --frames N "
		"[--allow-mainpath]\n", program);
	fprintf(stderr, "  --device PATH       V4L2 capture node (required)\n");
	fprintf(stderr, "  --width N           requested NV12 width (required)\n");
	fprintf(stderr, "  --height N          requested NV12 height (required)\n");
	fprintf(stderr, "  --frames N          requested MMAP buffer count, 1..%u (required)\n",
		MAX_REQUESTED_BUFFERS);
	fprintf(stderr, "  --allow-mainpath    explicitly permit /dev/video24\n");
}

static int parse_options(int argc, char **argv, struct probe_options *options)
{
	static const struct option long_options[] = {
		{"device", required_argument, NULL, 'd'},
		{"width", required_argument, NULL, 'w'},
		{"height", required_argument, NULL, 'h'},
		{"frames", required_argument, NULL, 'n'},
		{"allow-mainpath", no_argument, NULL, 'a'},
		{"help", no_argument, NULL, 'H'},
		{NULL, 0, NULL, 0},
	};
	int option;
	int have_width = 0;
	int have_height = 0;
	int have_frames = 0;

	memset(options, 0, sizeof(*options));
	while ((option = getopt_long(argc, argv, "d:w:h:n:aH", long_options,
					NULL)) != -1) {
		switch (option) {
		case 'd':
			options->device = optarg;
			break;
		case 'w':
			if (parse_positive_uint(optarg, UINT32_MAX, &options->width) != 0)
				return -1;
			have_width = 1;
			break;
		case 'h':
			if (parse_positive_uint(optarg, UINT32_MAX, &options->height) != 0)
				return -1;
			have_height = 1;
			break;
		case 'n':
			if (parse_positive_uint(optarg, MAX_REQUESTED_BUFFERS,
						&options->frames) != 0)
				return -1;
			have_frames = 1;
			break;
		case 'a':
			options->allow_mainpath = 1;
			break;
		case 'H':
			usage(argv[0]);
			return 1;
		default:
			return -1;
		}
	}
	if (optind != argc || !options->device || !options->device[0] ||
	    !have_width || !have_height || !have_frames || options->width % 2 != 0 ||
	    options->height % 2 != 0)
		return -1;
	if (rockiva_probe_is_mainpath(options->device, ROCKIVA_PROBE_MAINPATH) &&
	    !options->allow_mainpath) {
		fprintf(stderr,
			"refusing production mainpath %s; pass --allow-mainpath only "
			"for an explicitly approved experiment\n",
			options->device);
		return -1;
	}
	return 0;
}

static void fourcc_string(__u32 fourcc, char out[5])
{
	out[0] = (char)(fourcc & 0xffU);
	out[1] = (char)((fourcc >> 8) & 0xffU);
	out[2] = (char)((fourcc >> 16) & 0xffU);
	out[3] = (char)((fourcc >> 24) & 0xffU);
	out[4] = '\0';
}

static int query_capabilities(int fd)
{
	struct v4l2_capability capability;
	__u32 device_caps;

	memset(&capability, 0, sizeof(capability));
	if (ioctl_checked(fd, VIDIOC_QUERYCAP, &capability, "VIDIOC_QUERYCAP") != 0)
		return -1;
	device_caps = capability.capabilities;
	if (capability.capabilities & V4L2_CAP_DEVICE_CAPS)
		device_caps = capability.device_caps;
	printf("querycap driver=%s card=%s bus=%s version=0x%08x "
	       "capabilities=0x%08x device_caps=0x%08x\n",
	       capability.driver, capability.card, capability.bus_info,
	       capability.version, capability.capabilities, device_caps);
	if (!(device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)) {
		fprintf(stderr, "device lacks V4L2_CAP_VIDEO_CAPTURE_MPLANE\n");
		return -1;
	}
	if (!(device_caps & V4L2_CAP_STREAMING)) {
		fprintf(stderr, "device lacks V4L2_CAP_STREAMING\n");
		return -1;
	}
	return 0;
}

static int get_current_format(int fd, struct v4l2_format *format)
{
	memset(format, 0, sizeof(*format));
	format->type = V4L2_CAPTURE_TYPE;
	if (ioctl_checked(fd, VIDIOC_G_FMT, format, "VIDIOC_G_FMT") != 0)
		return -1;
	printf("format_before width=%u height=%u fourcc=0x%08x field=%u planes=%u\n",
	       format->fmt.pix_mp.width, format->fmt.pix_mp.height,
	       format->fmt.pix_mp.pixelformat, format->fmt.pix_mp.field,
	       format->fmt.pix_mp.num_planes);
	return 0;
}

static int negotiate_format(int fd, const struct probe_options *options,
				    struct v4l2_format *format)
{
	char actual_fourcc[5];
	int is_nv12;

	memset(format, 0, sizeof(*format));
	format->type = V4L2_CAPTURE_TYPE;
	format->fmt.pix_mp.width = options->width;
	format->fmt.pix_mp.height = options->height;
	format->fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
	format->fmt.pix_mp.field = V4L2_FIELD_NONE;
	if (ioctl_checked(fd, VIDIOC_S_FMT, format, "VIDIOC_S_FMT") != 0)
		return -1;
	fourcc_string(format->fmt.pix_mp.pixelformat, actual_fourcc);
	is_nv12 = format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12 ||
		format->fmt.pix_mp.pixelformat == V4L2_PIX_FMT_NV12M;
	printf("format requested=%ux%u NV12 negotiated=%ux%u fourcc=%s "
	       "field=%u planes=%u flags=0x%08x\n",
	       options->width, options->height, format->fmt.pix_mp.width,
	       format->fmt.pix_mp.height, actual_fourcc, format->fmt.pix_mp.field,
	       format->fmt.pix_mp.num_planes, format->fmt.pix_mp.flags);
	if (format->fmt.pix_mp.width != options->width ||
	    format->fmt.pix_mp.height != options->height || !is_nv12 ||
	    format->fmt.pix_mp.field != V4L2_FIELD_NONE) {
		fprintf(stderr, "VIDIOC_S_FMT did not retain progressive NV12/NV12M "
			"format\n");
		return -1;
	}
	if (format->fmt.pix_mp.num_planes == 0 ||
	    format->fmt.pix_mp.num_planes > VIDEO_MAX_PLANES) {
		fprintf(stderr, "negotiated plane count %u is outside 1..%u\n",
			format->fmt.pix_mp.num_planes, VIDEO_MAX_PLANES);
		return -1;
	}
	return 0;
}

static int request_buffers(int fd, unsigned int requested,
				   unsigned int *granted)
{
	struct v4l2_requestbuffers request;

	memset(&request, 0, sizeof(request));
	request.type = V4L2_CAPTURE_TYPE;
	request.memory = V4L2_MEMORY_TYPE;
	request.count = requested;
	if (ioctl_checked(fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS(MMAP)") != 0)
		return -1;
	*granted = request.count;
	if (request.count == 0 || request.count > MAX_REQUESTED_BUFFERS) {
		fprintf(stderr, "VIDIOC_REQBUFS returned unexpected buffer count %u\n",
			request.count);
		return -1;
	}
	printf("buffers requested=%u granted=%u memory=MMAP\n", requested,
	       *granted);
	return 0;
}

static int query_buffer(int fd, unsigned int index,
				struct queried_buffer *queried, unsigned int expected_planes)
{
	struct v4l2_buffer buffer;
	unsigned int plane;

	memset(queried, 0, sizeof(*queried));
	memset(&buffer, 0, sizeof(buffer));
	buffer.type = V4L2_CAPTURE_TYPE;
	buffer.memory = V4L2_MEMORY_TYPE;
	buffer.index = index;
	buffer.m.planes = queried->planes;
	buffer.length = VIDEO_MAX_PLANES;
	if (ioctl_checked(fd, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF") != 0)
		return -1;
	if (buffer.length == 0 || buffer.length > VIDEO_MAX_PLANES ||
	    buffer.length != expected_planes) {
		fprintf(stderr, "VIDIOC_QUERYBUF buffer=%u returned plane count %u, "
			"expected %u\n", index, buffer.length, expected_planes);
		return -1;
	}
	queried->plane_count = buffer.length;
	for (plane = 0; plane < queried->plane_count; plane++) {
		const struct v4l2_plane *entry = &queried->planes[plane];

		if (entry->length == 0) {
			fprintf(stderr, "VIDIOC_QUERYBUF buffer=%u plane=%u returned zero "
				"length\n", index, plane);
			return -1;
		}
		printf("query buffer=%u plane=%u length=%u bytesused=%u "
		       "mem_offset=0x%08x data_offset=%u\n", index, plane,
		       entry->length, entry->bytesused, entry->m.mem_offset,
		       entry->data_offset);
	}
	return 0;
}

static int validate_exported_fd(int exported_fd, unsigned int index,
					unsigned int plane)
{
	struct stat status;

	if (exported_fd < 0) {
		fprintf(stderr, "VIDIOC_EXPBUF buffer=%u plane=%u returned invalid fd %d\n",
			index, plane, exported_fd);
		return -1;
	}
	if (fcntl(exported_fd, F_GETFD) < 0) {
		int saved_errno = errno;

		fprintf(stderr, "export fd validation buffer=%u plane=%u fcntl failed: "
			"errno=%d (%s)\n", index, plane, saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	if (fstat(exported_fd, &status) < 0) {
		int saved_errno = errno;

		fprintf(stderr, "export fd validation buffer=%u plane=%u fstat failed: "
			"errno=%d (%s)\n", index, plane, saved_errno,
			strerror(saved_errno));
		errno = saved_errno;
		return -1;
	}
	printf("export buffer=%u plane=%u fd=%d valid=1 mode=0%o\n", index,
	       plane, exported_fd, status.st_mode & 07777);
	return 0;
}

static int export_buffer_plane(int fd, unsigned int index, unsigned int plane)
{
	struct v4l2_exportbuffer export_buffer;
	int exported_fd = -1;
	int result = -1;

	memset(&export_buffer, 0, sizeof(export_buffer));
	export_buffer.type = V4L2_CAPTURE_TYPE;
	export_buffer.index = index;
	export_buffer.plane = plane;
	export_buffer.flags = O_CLOEXEC | O_RDWR;
	if (ioctl_checked(fd, VIDIOC_EXPBUF, &export_buffer, "VIDIOC_EXPBUF") != 0)
		return -1;
	exported_fd = export_buffer.fd;
	if (validate_exported_fd(exported_fd, index, plane) != 0)
		goto done;
	result = 0;

	done:
	if (close_checked(&exported_fd, "close(exported DMA-BUF fd)") != 0)
		result = -1;
	return result;
}

static int release_queue(int fd, int stream_started)
{
	struct v4l2_requestbuffers request;
	int result = 0;

	/* STREAMOFF is attempted before REQBUFS(0), even though this probe never
	 * starts streaming.  EINVAL is the driver's normal "already stopped"
	 * response in that case. */
	if (ioctl_checked(fd, VIDIOC_STREAMOFF,
			  &(enum v4l2_buf_type){V4L2_CAPTURE_TYPE},
			  "VIDIOC_STREAMOFF") != 0) {
		if (!stream_started && errno == EINVAL) {
			fprintf(stderr, "VIDIOC_STREAMOFF returned EINVAL before STREAMON; "
				"treating the queue as already stopped\n");
		} else {
			/* Do not issue REQBUFS(0) while the queue state is unknown. */
			return -1;
		}
	}
	memset(&request, 0, sizeof(request));
	request.type = V4L2_CAPTURE_TYPE;
	request.memory = V4L2_MEMORY_TYPE;
	request.count = 0;
	if (ioctl_checked(fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS(0)") != 0)
		result = -1;
	return result;
}

static int restore_format(int fd, const struct v4l2_format *original)
{
	struct v4l2_format format;

	if (!original)
		return -1;
	format = *original;
	if (ioctl_checked(fd, VIDIOC_S_FMT, &format,
			  "VIDIOC_S_FMT(restore)") != 0)
		return -1;
	if (format.fmt.pix_mp.width != original->fmt.pix_mp.width ||
	    format.fmt.pix_mp.height != original->fmt.pix_mp.height ||
	    format.fmt.pix_mp.pixelformat != original->fmt.pix_mp.pixelformat ||
	    format.fmt.pix_mp.field != original->fmt.pix_mp.field ||
	    format.fmt.pix_mp.num_planes != original->fmt.pix_mp.num_planes) {
		fprintf(stderr, "VIDIOC_S_FMT(restore) did not retain the original format\n");
		return -1;
	}
	printf("format_restore=ok width=%u height=%u fourcc=0x%08x\n",
	       format.fmt.pix_mp.width, format.fmt.pix_mp.height,
	       format.fmt.pix_mp.pixelformat);
	return 0;
}

int main(int argc, char **argv)
{
	struct probe_options options;
	struct v4l2_format original_format;
	struct v4l2_format format;
	struct queried_buffer *buffers = NULL;
	unsigned int buffer_count = 0;
	unsigned int buffer;
	unsigned int plane;
	int fd = -1;
	int queue_allocated = 0;
	int stream_started = 0;
	int format_restore_required = 0;
	int v4l2_cleanup_safe = 1;
	int opened_mainpath;
	int result = 1;
	int parse_result;

	parse_result = parse_options(argc, argv, &options);
	if (parse_result != 0) {
		if (parse_result < 0)
			usage(argv[0]);
		return parse_result > 0 ? 0 : 2;
	}
	printf("probe mode=v4l2-expbuf device=%s width=%u height=%u "
	       "requested_buffers=%u\n", options.device, options.width,
	       options.height, options.frames);
	fd = open(options.device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0) {
		int saved_errno = errno;

		fprintf(stderr, "open %s failed: errno=%d (%s)\n", options.device,
			saved_errno, strerror(saved_errno));
		return 1;
	}
	opened_mainpath = rockiva_probe_fd_is_mainpath(fd,
							ROCKIVA_PROBE_MAINPATH);
	if (opened_mainpath < 0) {
		int saved_errno = errno;

		fprintf(stderr, "cannot verify opened V4L2 device identity: errno=%d (%s)\n",
			saved_errno, strerror(saved_errno));
		errno = saved_errno;
		goto cleanup;
	}
	if (opened_mainpath && !options.allow_mainpath) {
		fprintf(stderr,
			"refusing opened production mainpath %s after identity check; pass "
			"--allow-mainpath only for an explicitly approved experiment\n",
			options.device);
		goto cleanup;
	}
	if (query_capabilities(fd) != 0 ||
	    get_current_format(fd, &original_format) != 0)
		goto cleanup;
	format_restore_required = 1;
	if (negotiate_format(fd, &options, &format) != 0)
		goto cleanup;
	/* A successful REQBUFS ioctl may allocate a queue even when the returned
	 * count is unexpected, so cleanup still runs in that case. */
	if (request_buffers(fd, options.frames, &buffer_count) != 0) {
		if (buffer_count != 0)
			queue_allocated = 1;
		goto cleanup;
	}
	queue_allocated = 1;
	buffers = calloc(buffer_count, sizeof(*buffers));
	if (!buffers) {
		fprintf(stderr, "calloc query buffer table failed\n");
		goto cleanup;
	}
	for (buffer = 0; buffer < buffer_count; buffer++) {
		if (query_buffer(fd, buffer, &buffers[buffer],
					format.fmt.pix_mp.num_planes) != 0)
			goto cleanup;
		for (plane = 0; plane < buffers[buffer].plane_count; plane++) {
			if (export_buffer_plane(fd, buffer, plane) != 0)
				goto cleanup;
		}
	}
	printf("result=V4L2_EXPBUF_CAPABILITY_OK buffers=%u physical_planes=%u "
	       "single_fd_candidate=%u rockiva=not_run inference=not_run "
	       "t1=not_claimed\n", buffer_count, format.fmt.pix_mp.num_planes,
	       format.fmt.pix_mp.num_planes == 1 ? 1U : 0U);
	result = 0;

cleanup:
	if (queue_allocated && release_queue(fd, stream_started) != 0) {
		result = 1;
		v4l2_cleanup_safe = 0;
	}
	if (format_restore_required && v4l2_cleanup_safe &&
	    restore_format(fd, &original_format) != 0)
		result = 1;
	free(buffers);
	if (fd >= 0 && v4l2_cleanup_safe &&
	    close_checked(&fd, "close(V4L2 device)") != 0)
		result = 1;
	if (!v4l2_cleanup_safe && fd >= 0)
		fprintf(stderr, "leaving V4L2 device open until process exit after "
			"queue teardown failure\n");
	if (result != 0)
		fprintf(stderr, "result=V4L2_EXPBUF_CAPABILITY_FAILED rockiva=not_run "
			"inference=not_run t1=not_claimed\n");
	return result;
}
