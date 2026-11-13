#ifndef FLOW_DMA_H
#define FLOW_DMA_H
#include <gst/gst.h>
int flow_dma_init(void);
void flow_dma_deinit(void);
GstBufferPool *flow_dma_pool(GstCaps *caps, int width, int height);
int flow_dma_blit(GstSample *sample, GstBuffer *output, int rotation, int input_bgr, int width, int height);
int flow_dma_cpu_sync(GstBuffer *buffer, int start);
#endif
