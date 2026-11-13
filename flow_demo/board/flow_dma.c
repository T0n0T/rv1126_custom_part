/* DMA-BUF preview: RGA writes the exact buffer subsequently imported by KMS. */
#define _POSIX_C_SOURCE 200809L
#include "flow_dma.h"
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>
#include <rga/RgaApi.h>
#include <rga/drmrga.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <math.h>
#include <sys/ioctl.h>
#include <unistd.h>

typedef struct { GstAllocator parent; } FlowDmaAllocator;
typedef struct { GstAllocatorClass parent; } FlowDmaAllocatorClass;
GType flow_dma_allocator_get_type(void);
G_DEFINE_TYPE(FlowDmaAllocator, flow_dma_allocator, GST_TYPE_ALLOCATOR)

static GstMemory *dma_allocate(GstAllocator *allocator, gsize size, GstAllocationParams *params)
{
	struct dma_heap_allocation_data request = {0};
	const char *heaps[] = {"/dev/dma_heap/system-uncached", "/dev/dma_heap/system"};
	int fd = -1;
	(void)allocator; (void)params;
	for (size_t i = 0; i < G_N_ELEMENTS(heaps); ++i) {
		fd = open(heaps[i], O_RDWR | O_CLOEXEC);
		if (fd >= 0) break;
	}
	if (fd < 0) return NULL;
	request.len = size;
	request.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(fd, DMA_HEAP_IOCTL_ALLOC, &request) < 0) { close(fd); return NULL; }
	close(fd);
	GstAllocator *wrapper = gst_dmabuf_allocator_new();
	GstMemory *memory = gst_dmabuf_allocator_alloc(wrapper, (int)request.fd, size);
	gst_object_unref(wrapper);
	if (!memory) close((int)request.fd);
	return memory;
}

static void flow_dma_allocator_class_init(FlowDmaAllocatorClass *klass)
{
	GST_ALLOCATOR_CLASS(klass)->alloc = dma_allocate;
}
static void flow_dma_allocator_init(FlowDmaAllocator *allocator)
{
	GST_ALLOCATOR(allocator)->mem_type = "flow-demo-dma-heap";
	GST_OBJECT_FLAG_SET(allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC | GST_ALLOCATOR_FLAG_NO_COPY);
}

int flow_dma_init(void) { return c_RkRgaInit(); }
void flow_dma_deinit(void) { c_RkRgaDeInit(); }

GstBufferPool *flow_dma_pool(GstCaps *caps, int width, int height)
{
	GstBufferPool *pool = gst_video_buffer_pool_new();
	GstStructure *config = gst_buffer_pool_get_config(pool);
	GstAllocator *allocator = g_object_new(flow_dma_allocator_get_type(), NULL);
	GstAllocationParams params;
	gst_allocation_params_init(&params);
	gst_buffer_pool_config_set_params(config, caps, (guint)((size_t)width * height * 4), 3, 4);
	gst_buffer_pool_config_set_allocator(config, allocator, &params);
	gst_buffer_pool_config_add_option(config, GST_BUFFER_POOL_OPTION_VIDEO_META);
	gst_object_unref(allocator);
	if (!gst_buffer_pool_set_config(pool, config) || !gst_buffer_pool_set_active(pool, TRUE)) {
		gst_object_unref(pool);
		return NULL;
	}
	return pool;
}

static int buffer_fd(GstBuffer *buffer)
{
	if (gst_buffer_n_memory(buffer) != 1) return -1;
	GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
	if (!gst_is_dmabuf_memory(memory)) return -1;
	gsize offset = 0;
	(void)gst_memory_get_sizes(memory, &offset, NULL);
	if (offset) return -1;
	return gst_dmabuf_memory_get_fd(memory);
}

int flow_dma_cpu_sync(GstBuffer *buffer, int start)
{
	int fd = buffer_fd(buffer);
	struct dma_buf_sync sync = {.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) | DMA_BUF_SYNC_RW};
	return fd < 0 ? -1 : ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

int flow_dma_blit(GstSample *sample, GstBuffer *output, int rotation, int input_bgr, int width, int height)
{
	GstBuffer *input = gst_sample_get_buffer(sample);
	GstVideoInfo info;
	GstVideoMeta *in_meta = gst_buffer_get_video_meta(input);
	GstVideoMeta *out_meta = gst_buffer_get_video_meta(output);
	rga_info_t source = {0}, dest = {0};
	if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
	    GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGB || !out_meta) return -1;
	int iw = GST_VIDEO_INFO_WIDTH(&info), ih = GST_VIDEO_INFO_HEIGHT(&info);
	int stride = in_meta ? in_meta->stride[0] : GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
	int rw = rotation % 180 ? ih : iw, rh = rotation % 180 ? iw : ih;
	double scale = fmin((double)width / rw, (double)height / rh);
	int ow = (int)lrint(rw * scale), oh = (int)lrint(rh * scale);
	source.fd = buffer_fd(input); dest.fd = buffer_fd(output);
	if (source.fd < 0 || dest.fd < 0 || stride % 3 || out_meta->stride[0] % 4) {
		g_printerr("flow_dma requires single DMA-BUF RGB input and BGRx output with valid strides\n");
		return -1;
	}
	source.mmuFlag = dest.mmuFlag = 1;
	source.rotation = rotation == 90 ? HAL_TRANSFORM_ROT_90 : rotation == 180 ? HAL_TRANSFORM_ROT_180 :
		rotation == 270 ? HAL_TRANSFORM_ROT_270 : 0;
	rga_set_rect(&dest.rect, 0, 0, width, height, out_meta->stride[0] / 4, height, RK_FORMAT_BGRA_8888);
	dest.color = 0xff000000;
	if (c_RkRgaColorFill(&dest) < 0) return -1;
	rga_set_rect(&source.rect, 0, 0, iw, ih, stride / 3, ih,
		input_bgr ? RK_FORMAT_BGR_888 : RK_FORMAT_RGB_888);
	rga_set_rect(&dest.rect, (width - ow) / 2, (height - oh) / 2, ow, oh,
		out_meta->stride[0] / 4, height, RK_FORMAT_BGRA_8888);
	return c_RkRgaBlit(&source, &dest, NULL);
}
