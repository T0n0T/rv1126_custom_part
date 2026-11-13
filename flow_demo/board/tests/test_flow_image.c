#include "flow_image.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *pixel(const uint8_t *input, int x, int y)
{
	return input + ((size_t)y * FLOW_INPUT_WIDTH + x) * 3;
}

static void check_panel_rotation(const struct flow_overlay *overlay)
{
	const int width = 800, height = 480;
	uint8_t *logical = malloc((size_t)width * height * 4);
	assert(logical);
	memset(logical, 0x5a, (size_t)width * height * 4);
	flow_draw_overlay(logical, width, height, width * 4, 640, 360, overlay, 30, 1200);
	for (int rotation = 0; rotation < 360; rotation += 90) {
		int nw = rotation % 180 ? height : width;
		int nh = rotation % 180 ? width : height;
		int stride = nw * 4 + 12;
		uint8_t *native = malloc((size_t)stride * nh);
		assert(native);
		memset(native, 0x5a, (size_t)stride * nh);
		assert(flow_draw_overlay_rotated(native, nw, nh, stride, rotation, 640, 360, overlay, 30, 1200) == 0);
		/* Compare a complete UI against rotating the rendered logical image. */
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x) {
				int nx = x, ny = y;
				if (rotation == 90) { nx = height - 1 - y; ny = x; }
				if (rotation == 180) { nx = width - 1 - x; ny = height - 1 - y; }
				if (rotation == 270) { nx = y; ny = width - 1 - x; }
				assert(memcmp(logical + ((size_t)y * width + x) * 4,
					native + (size_t)ny * stride + nx * 4, 4) == 0);
			}
		for (int y = 0; y < nh; ++y)
			for (int x = nw * 4; x < stride; ++x) assert(native[y * stride + x] == 0x5a);
		assert(flow_draw_overlay_rotated(native, nw, nh, stride, 45, 640, 360, overlay, 30, 1200) == -1);
		free(native);
	}
	free(logical);
}

int main(void)
{
	uint8_t *source = malloc(640 * 360 * 3);
	uint8_t *input = malloc(FLOW_INPUT_BYTES);
	uint8_t *bgr_input = malloc(FLOW_INPUT_BYTES);
	uint8_t *screen = malloc(480 * 800 * 4);
	struct frame_metadata metadata = {0};
	const int rotations[] = {0, 90, 180, 270};
	/* Quadrants make rotations visible; a padded stride exercises camera layout. */
	uint8_t *padded = malloc(1936 * 360);
	uint8_t *bgr_padded = malloc(1936 * 360);
	assert(source && input && bgr_input && screen && padded && bgr_padded);
	for (int y = 0; y < 360; ++y)
		for (int x = 0; x < 640; ++x) {
			uint8_t *p = source + ((size_t)y * 640 + x) * 3;
			p[0] = x < 320 ? 255 : 0;
			p[1] = y < 180 ? 255 : 0;
			p[2] = 33;
		}
	for (int y = 0; y < 360; ++y) memcpy(padded + y * 1936, source + y * 1920, 1920);
	for (int y = 0; y < 360; ++y)
		for (int x = 0; x < 640; ++x) {
			const uint8_t *rgb = source + ((size_t)y * 640 + x) * 3;
			uint8_t *bgr = bgr_padded + y * 1936 + x * 3;
			bgr[0] = rgb[2]; bgr[1] = rgb[1]; bgr[2] = rgb[0];
		}
	for (int i = 0; i < 4; ++i) {
		int x, y;
		const uint8_t *p;
		assert(flow_letterbox_rgb(padded, 640, 360, 1936, rotations[i], input, &metadata) == 0);
		struct frame_metadata bgr_metadata = {0};
		assert(flow_letterbox_bgr(bgr_padded, 640, 360, 1936, rotations[i], bgr_input, &bgr_metadata) == 0);
		assert(memcmp(input, bgr_input, FLOW_INPUT_BYTES) == 0);
		assert(bgr_metadata.source_width == metadata.source_width && bgr_metadata.source_height == metadata.source_height);
		assert(metadata.resize_scale == 1);
		assert(metadata.source_width == (i % 2 ? 360U : 640U));
		assert(metadata.source_height == (i % 2 ? 640U : 360U));
		assert(metadata.pad_left == (i % 2 ? 140 : 0));
		assert(metadata.pad_top == (i % 2 ? 0 : 140));
		/* The source top-left corner moves clockwise with each rotation. */
		x = metadata.pad_left + ((i == 1 || i == 2) ? (int)metadata.source_width - 1 : 0);
		y = metadata.pad_top + ((i == 2 || i == 3) ? (int)metadata.source_height - 1 : 0);
		p = pixel(input, x, y);
		assert(p[0] == 255 && p[1] == 255 && p[2] == 33);
		p = pixel(input, 0, 0);
		assert(p[0] == 114 && p[1] == 114 && p[2] == 114);
	}
	assert(flow_letterbox_rgb(source, 640, 360, 1920, 90, input, &metadata) == 0);
	struct flow_detection box = {{0.25, 0.25, 0.75, 0.75}, 0.8f, 0};
	flow_render_bgrx(input, &metadata, &box, 1, screen, 480, 800, 7, 128);
	/* 360:640 image fits at 450x800, centered with 15px side bars. */
	assert(screen[(400 * 480 + 0) * 4] == 0);
	int bx = 15 + (int)(0.25 * 449 + 0.5);
	int by = (int)(0.25 * 799 + 0.5);
	const uint8_t *edge = screen + ((size_t)(by + 50) * 480 + bx) * 4;
	assert(edge[0] == 0x40 && edge[1] == 0xff && edge[2] == 0x40);
	/* The live overlay labels delayed results and hides stale boxes. */
	struct flow_overlay overlay = {.enabled = 1, .pts_ms = 1000, .count = 1, .take_out = 1, .put_in = 1};
	overlay.tracks[0].detection = box;
	overlay.tracks[0].track_id = 7;
	overlay.tracks[0].zone = 2;
	overlay.zones[0] = (struct flow_polygon){.count = 4, .xy = {{0,0}, {0.3,0}, {0.3,1}, {0,1}}};
	overlay.zones[1] = (struct flow_polygon){.count = 4, .xy = {{0.3,0}, {0.6,0}, {0.6,1}, {0.3,1}}};
	overlay.zones[2] = (struct flow_polygon){.count = 4, .xy = {{0.6,0}, {1,0}, {1,1}, {0.6,1}}};
	strcpy(overlay.last_event, "TAKE OUT ID 7");
	strcpy(overlay.reason, "observation_gap_exceeded");
	check_panel_rotation(&overlay);
	memset(screen, 0, 480 * 800 * 4);
	flow_draw_overlay(screen, 480, 800, 480 * 4, 360, 640, &overlay, 30, 1200);
	assert(edge[0] == 0x40 && edge[1] == 0xff && edge[2] == 0x40);
	memset(screen, 0, 480 * 800 * 4);
	flow_draw_overlay(screen, 480, 800, 480 * 4, 360, 640, &overlay, 30, 1500);
	assert(edge[0] == 0 && edge[1] == 0 && edge[2] == 0);
	assert(flow_letterbox_rgb(source, 640, 360, 1900, 0, input, &metadata) == -1);
	assert(flow_letterbox_rgb(source, 640, 360, 1920, 45, input, &metadata) == -1);
	/* A larger capture uses the interpolation path, not the ISP-size copy path. */
	uint8_t *large = malloc(1280 * 720 * 3);
	assert(large);
	for (int y = 0; y < 720; ++y)
		for (int x = 0; x < 1280; ++x)
			memcpy(large + ((size_t)y * 1280 + x) * 3,
			       source + ((size_t)(y / 2) * 640 + x / 2) * 3, 3);
	assert(flow_letterbox_rgb(large, 1280, 720, 3840, 270, input, &metadata) == 0);
	for (int i = 0; i < 1280 * 720; ++i) {
		uint8_t red = large[i * 3]; large[i * 3] = large[i * 3 + 2]; large[i * 3 + 2] = red;
	}
	assert(flow_letterbox_bgr(large, 1280, 720, 3840, 270, bgr_input, &metadata) == 0);
	assert(memcmp(input, bgr_input, FLOW_INPUT_BYTES) == 0);
	assert(metadata.resize_scale == 0.5 && metadata.pad_left == 140 && metadata.pad_top == 0);
	const uint8_t *corner = pixel(input, 140, 639);
	assert(corner[0] == 255 && corner[1] == 255 && corner[2] == 33);
	free(large);
	free(source); free(input); free(bgr_input); free(screen); free(padded); free(bgr_padded);
	return 0;
}
