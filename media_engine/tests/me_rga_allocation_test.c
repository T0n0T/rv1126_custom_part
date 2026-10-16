#define _POSIX_C_SOURCE 200809L

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int test_access(const char *path, int mode);

#define access test_access
#include "../src/gst/me_rga_rotate.c"
#undef access

static int test_access(const char *path, int mode)
{
	(void)mode;
	return strcmp(path, ME_RGA_DMA_HEAP_UNCACHED) == 0 ? 0 : -1;
}

static void mark_destroyed(gpointer data, GObject *object)
{
	bool *destroyed = data;

	(void)object;
	*destroyed = true;
}

static void check(bool condition, const char *message)
{
	if (!condition) {
		fprintf(stderr, "me_rga_allocation_test: %s\n", message);
		exit(1);
	}
}

static GstCaps *test_caps(void)
{
	return gst_caps_from_string(
	    "video/x-raw,format=NV12,width=64,height=48,framerate=30/1");
}

static void test_decide_existing_pool(GstBaseTransform *transform)
{
	GstCaps *caps = test_caps();
	GstAllocator *allocator = gst_dmabuf_allocator_new();
	GstBufferPool *pool = gst_video_buffer_pool_new();
	GstStructure *config = gst_buffer_pool_get_config(pool);
	GstQuery *query = gst_query_new_allocation(caps, TRUE);
	GstAllocationParams params;
	bool allocator_destroyed = false;
	bool pool_destroyed = false;

	g_object_weak_ref(G_OBJECT(allocator), mark_destroyed,
	                  &allocator_destroyed);
	g_object_weak_ref(G_OBJECT(pool), mark_destroyed, &pool_destroyed);
	gst_allocation_params_init(&params);
	gst_buffer_pool_config_set_params(config, caps, 4608, 0, 0);
	gst_buffer_pool_config_set_allocator(config, allocator, &params);
	check(gst_buffer_pool_set_config(pool, config),
	      "could not configure existing pool");
	gst_query_add_allocation_param(query, allocator, &params);
	gst_query_add_allocation_pool(query, pool, 4608, 0, 0);
	gst_object_unref(allocator);

	check(me_rga_rotate_decide_allocation(transform, query),
	      "decide_allocation failed for existing pool");
	check(!allocator_destroyed,
	      "allocator finalized while allocation query still owns it");
	gst_query_unref(query);
	check(allocator_destroyed,
	      "original allocator leaked after allocation query release");
	check(!pool_destroyed, "decide_allocation consumed caller's pool reference");
	gst_object_unref(pool);
	check(pool_destroyed, "decide_allocation leaked parsed pool reference");
	gst_caps_unref(caps);
}

static void test_decide_new_pool(GstBaseTransform *transform)
{
	GstCaps *caps = test_caps();
	GstQuery *query = gst_query_new_allocation(caps, TRUE);
	GstBufferPool *pool = NULL;
	bool pool_destroyed = false;

	check(me_rga_rotate_decide_allocation(transform, query),
	      "decide_allocation failed without downstream pool");
	check(gst_query_get_n_allocation_pools(query) == 1,
	      "decide_allocation did not create an output pool");
	gst_query_parse_nth_allocation_pool(query, 0, &pool, NULL, NULL, NULL);
	check(pool != NULL, "created output pool is NULL");
	g_object_weak_ref(G_OBJECT(pool), mark_destroyed, &pool_destroyed);
	gst_object_unref(pool);
	check(!pool_destroyed, "query lost ownership of new output pool");
	gst_query_unref(query);
	check(pool_destroyed, "new output pool leaked after query release");
	gst_caps_unref(caps);
}

static void test_propose_existing_allocator(GstBaseTransform *transform)
{
	GstCaps *caps = test_caps();
	GstAllocator *allocator = gst_dmabuf_allocator_new();
	GstQuery *decide_query = gst_query_new_allocation(caps, TRUE);
	GstQuery *query = gst_query_new_allocation(caps, TRUE);
	GstAllocationParams params;
	bool allocator_destroyed = false;

	g_object_weak_ref(G_OBJECT(allocator), mark_destroyed,
	                  &allocator_destroyed);
	gst_allocation_params_init(&params);
	gst_query_add_allocation_param(query, allocator, &params);
	gst_object_unref(allocator);
	check(me_rga_rotate_propose_allocation(transform, decide_query, query),
	      "propose_allocation failed with existing allocator");
	check(!allocator_destroyed,
	      "propose_allocation prematurely released allocator");
	check(gst_query_get_n_allocation_pools(query) == 1,
	      "propose_allocation did not create an input pool");
	gst_query_unref(query);
	check(allocator_destroyed,
	      "propose_allocation leaked parsed allocator reference");
	gst_query_unref(decide_query);
	gst_caps_unref(caps);
}

int main(void)
{
	GstBaseTransform *transform;
	int iteration;

	gst_init(NULL, NULL);
	transform = g_object_new(me_rga_rotate_get_type(), NULL);
	check(transform != NULL, "could not create real rgarotate element");
	for (iteration = 0; iteration < 100; iteration++) {
		test_decide_existing_pool(transform);
		test_decide_new_pool(transform);
		test_propose_existing_allocator(transform);
	}
	gst_object_unref(transform);
	puts("me_rga_allocation_test: PASS (100 allocation/cleanup cycles)");
	return 0;
}
