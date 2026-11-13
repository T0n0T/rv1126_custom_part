/* RGB preprocessing and a small screen overlay, independent of GStreamer. */
#include "flow_image.h"

#include <math.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t *rotated_pixel(const uint8_t *source, int width, int height,
				   int stride, int rotation, int x, int y)
{
	int sx = x, sy = y;
	switch (rotation) {
	case 90: sx = y; sy = height - 1 - x; break;
	case 180: sx = width - 1 - x; sy = height - 1 - y; break;
	case 270: sx = width - 1 - y; sy = x; break;
	}
	return source + (size_t)sy * stride + (size_t)sx * 3;
}

static int letterbox(const uint8_t *source, int width, int height, int stride,
		       int rotation, int bgr, uint8_t *input, struct frame_metadata *metadata)
{
	int rw, rh, ow, oh;
	double scale;
	if (!source || !input || !metadata || width <= 0 || width > 8192 ||
	    height <= 0 || height > 8192 || stride < width * 3 ||
	    (rotation != 0 && rotation != 90 && rotation != 180 && rotation != 270))
		return -1;
	rw = rotation % 180 ? height : width;
	rh = rotation % 180 ? width : height;
	scale = fmin((double)FLOW_INPUT_WIDTH / rw, (double)FLOW_INPUT_HEIGHT / rh);
	ow = (int)lrint(rw * scale);
	oh = (int)lrint(rh * scale);
	if (ow < 1 || oh < 1)
		return -1;
	metadata->source_width = (uint32_t)rw;
	metadata->source_height = (uint32_t)rh;
	metadata->resize_scale = scale;
	metadata->pad_left = (FLOW_INPUT_WIDTH - ow) / 2;
	metadata->pad_top = (FLOW_INPUT_HEIGHT - oh) / 2;
	memset(input, 114, FLOW_INPUT_BYTES);
	if (ow == rw && oh == rh) {
		/* ISP already supplies the model's working size on the default camera path. */
		for (int y = 0; y < rh; ++y) {
			uint8_t *out = input + ((size_t)(y + metadata->pad_top) * FLOW_INPUT_WIDTH + metadata->pad_left) * 3;
			if (rotation == 0 && !bgr) {
				memcpy(out, source + (size_t)y * stride, (size_t)rw * 3);
			} else {
				for (int x = 0; x < rw; ++x) {
					const uint8_t *p = rotated_pixel(source, width, height, stride, rotation, x, y);
					out[x * 3] = p[bgr ? 2 : 0];
					out[x * 3 + 1] = p[1];
					out[x * 3 + 2] = p[bgr ? 0 : 2];
				}
			}
		}
		return 0;
	}
	for (int y = 0; y < oh; ++y) {
		double sy = fmax(0, (y + 0.5) * rh / oh - 0.5);
		int y0 = (int)sy, y1 = y0 + 1 < rh ? y0 + 1 : y0;
		double fy = sy - y0;
		for (int x = 0; x < ow; ++x) {
			double sx = fmax(0, (x + 0.5) * rw / ow - 0.5);
			int x0 = (int)sx, x1 = x0 + 1 < rw ? x0 + 1 : x0;
			double fx = sx - x0;
			const uint8_t *a = rotated_pixel(source, width, height, stride, rotation, x0, y0);
			const uint8_t *b = rotated_pixel(source, width, height, stride, rotation, x1, y0);
			const uint8_t *c = rotated_pixel(source, width, height, stride, rotation, x0, y1);
			const uint8_t *d = rotated_pixel(source, width, height, stride, rotation, x1, y1);
			uint8_t *out = input + ((size_t)(y + metadata->pad_top) * FLOW_INPUT_WIDTH +
					       x + metadata->pad_left) * 3;
			for (int channel = 0; channel < 3; ++channel) {
				int sc = bgr ? 2 - channel : channel;
				out[channel] = (uint8_t)lrint((1 - fy) * ((1 - fx) * a[sc] + fx * b[sc]) +
							     fy * ((1 - fx) * c[sc] + fx * d[sc]));
			}
		}
	}
	return 0;
}

int flow_letterbox_rgb(const uint8_t *source, int width, int height, int stride,
		       int rotation, uint8_t *input, struct frame_metadata *metadata)
{
	return letterbox(source, width, height, stride, rotation, 0, input, metadata);
}

int flow_letterbox_bgr(const uint8_t *source, int width, int height, int stride,
		       int rotation, uint8_t *input, struct frame_metadata *metadata)
{
	return letterbox(source, width, height, stride, rotation, 1, input, metadata);
}

struct drawing_surface {
	uint8_t *origin;
	ptrdiff_t x_step;
	ptrdiff_t y_step;
	int width;
	int height;
};

static struct drawing_surface drawing_surface(uint8_t *screen, int width, int height, int stride, int rotation)
{
	struct drawing_surface surface = {screen, 4, stride, width, height};
	if (rotation % 180) { surface.width = height; surface.height = width; }
	switch (rotation) {
	case 90:
		surface.origin += (ptrdiff_t)(width - 1) * 4;
		surface.x_step = stride; surface.y_step = -4;
		break;
	case 180:
		surface.origin += (ptrdiff_t)(height - 1) * stride + (ptrdiff_t)(width - 1) * 4;
		surface.x_step = -4; surface.y_step = -stride;
		break;
	case 270:
		surface.origin += (ptrdiff_t)(height - 1) * stride;
		surface.x_step = -stride; surface.y_step = 4;
		break;
	}
	return surface;
}

static void rectangle(const struct drawing_surface *surface,
		       int x, int y, int w, int h, uint32_t color)
{
	int right = x + w < surface->width ? x + w : surface->width;
	int bottom = y + h < surface->height ? y + h : surface->height;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	for (int row = y; row < bottom; ++row)
		for (int col = x; col < right; ++col) {
			uint8_t *p = surface->origin + row * surface->y_step + col * surface->x_step;
			p[0] = (uint8_t)color;
			p[1] = (uint8_t)(color >> 8);
			p[2] = (uint8_t)(color >> 16);
			p[3] = 255;
		}
}

/* 5x7 glyph rows, sufficient for readable class names, scores and status. */
static const char glyph_chars[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.-:#/ ";
static const uint8_t glyphs[][7] = {
	{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
	{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
	{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
	{14,17,17,15,1,1,14},{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},
	{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
	{31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},
	{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},{17,18,20,24,20,18,17},
	{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},
	{14,17,17,17,17,17,14},{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},
	{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
	{17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},
	{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
	{0,0,0,0,0,12,12},{0,0,0,31,0,0,0},{0,12,12,0,12,12,0},
	{10,31,10,10,31,10,0},{1,2,2,4,8,8,16},{0,0,0,0,0,0,0},
};

static void text(const struct drawing_surface *surface, int x, int y,
		 const char *label, uint32_t color)
{
	for (const char *p = label; *p; ++p, x += 12) {
		const char *glyph = strchr(glyph_chars, *p);
		if (!glyph) continue;
		for (int row = 0; row < 7; ++row)
			for (int col = 0; col < 5; ++col)
				if (glyphs[glyph - glyph_chars][row] & (1U << (4 - col)))
					rectangle(surface, x + col * 2, y + row * 2, 2, 2, color);
	}
}

void flow_render_bgrx(const uint8_t *input, const struct frame_metadata *metadata,
		      const struct flow_detection *detections, size_t count,
		      uint8_t *screen, int width, int height, double fps, double infer_ms)
{
	double scale = fmin((double)width / metadata->source_width,
			    (double)height / metadata->source_height);
	int ow = (int)lrint(metadata->source_width * scale);
	int oh = (int)lrint(metadata->source_height * scale);
	int left = (width - ow) / 2, top = (height - oh) / 2;
	int rw = (int)lrint(metadata->source_width * metadata->resize_scale);
	int rh = (int)lrint(metadata->source_height * metadata->resize_scale);
	size_t classes[2] = {0, 0};
	char label[128];
	struct drawing_surface surface = drawing_surface(screen, width, height, width * 4, 0);
	memset(screen, 0, (size_t)width * height * 4);
	for (int y = 0; y < oh; ++y)
		for (int x = 0; x < ow; ++x) {
			const uint8_t *p = input + ((size_t)(metadata->pad_top + y * rh / oh) * FLOW_INPUT_WIDTH +
						   metadata->pad_left + x * rw / ow) * 3;
			uint8_t *out = screen + ((size_t)(top + y) * width + left + x) * 4;
			out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; out[3] = 255;
		}
	for (size_t i = 0; i < count; ++i) {
		const struct flow_detection *d = detections + i;
		uint32_t color = d->class_id == 0 ? 0x40ff40U : 0x40dfffU;
		int x1 = left + (int)lrint(d->xyxy[0] * (ow - 1));
		int y1 = top + (int)lrint(d->xyxy[1] * (oh - 1));
		int x2 = left + (int)lrint(d->xyxy[2] * (ow - 1));
		int y2 = top + (int)lrint(d->xyxy[3] * (oh - 1));
		int ly = y1 >= 18 ? y1 - 18 : y1 + 3;
		if (d->class_id < 2) ++classes[d->class_id];
		rectangle(&surface, x1, y1, x2 - x1 + 1, 3, color);
		rectangle(&surface, x1, y2 - 2, x2 - x1 + 1, 3, color);
		rectangle(&surface, x1, y1, 3, y2 - y1 + 1, color);
		rectangle(&surface, x2 - 2, y1, 3, y2 - y1 + 1, color);
		snprintf(label, sizeof(label), "%s %.2f", d->class_id == 0 ? "HELMET" : "HEAD", d->score);
		rectangle(&surface, x1, ly, (int)strlen(label) * 12, 18, 0);
		text(&surface, x1, ly + 2, label, color);
	}
	rectangle(&surface, 0, 0, width, 38, 0);
	snprintf(label, sizeof(label), "LIVE HELMET %zu HEAD %zu", classes[0], classes[1]);
	text(&surface, 6, 3, label, 0xffffffU);
	snprintf(label, sizeof(label), "FPS %.1f NPU %.0f MS", fps, infer_ms);
	text(&surface, 6, 21, label, 0xffdf60U);
}

int flow_save_ppm(const char *path, const uint8_t *screen, int width, int height)
{
	FILE *file = fopen(path, "wx");
	int status = 0;
	if (!file) return -1;
	if (fprintf(file, "P6\n%d %d\n255\n", width, height) < 0) status = -1;
	for (size_t i = 0; status == 0 && i < (size_t)width * height; ++i) {
		const uint8_t rgb[3] = {screen[4 * i + 2], screen[4 * i + 1], screen[4 * i]};
		if (fwrite(rgb, 1, 3, file) != 3) status = -1;
	}
	if (fclose(file) != 0) status = -1;
	return status;
}

static void line(const struct drawing_surface *surface, int x0, int y0, int x1, int y1, uint32_t color)
{
	int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
	int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, error = dx + dy;
	for (;;) {
		rectangle(surface, x0, y0, 2, 2, color);
		if (x0 == x1 && y0 == y1) break;
		int twice = error * 2;
		if (twice >= dy) { error += dy; x0 += sx; }
		if (twice <= dx) { error += dx; y0 += sy; }
	}
}

static int save_rotated_ppm(const char *path, const uint8_t *source, int width, int height, int stride, int rotation, int bgr)
{
	int rw = rotation % 180 ? height : width, rh = rotation % 180 ? width : height;
	uint8_t *row = malloc((size_t)rw * 3);
	FILE *file = fopen(path, "wx");
	int status = -1;
	if (file && row && fprintf(file, "P6\n%d %d\n255\n", rw, rh) > 0) {
		status = 0;
		for (int y = 0; y < rh && status == 0; ++y) {
			for (int x = 0; x < rw; ++x) {
				const uint8_t *p = rotated_pixel(source, width, height, stride, rotation, x, y);
				row[x * 3] = p[bgr ? 2 : 0];
				row[x * 3 + 1] = p[1];
				row[x * 3 + 2] = p[bgr ? 0 : 2];
			}
			if (fwrite(row, 3, (size_t)rw, file) != (size_t)rw) status = -1;
		}
	}
	if (file && fclose(file) != 0) status = -1;
	free(row);
	return status;
}

int flow_save_rotated_ppm(const char *path, const uint8_t *source, int width, int height, int stride, int rotation)
{
	return save_rotated_ppm(path, source, width, height, stride, rotation, 0);
}

int flow_save_rotated_bgr_ppm(const char *path, const uint8_t *source, int width, int height, int stride, int rotation)
{
	return save_rotated_ppm(path, source, width, height, stride, rotation, 1);
}

void flow_draw_overlay(uint8_t *screen, int width, int height, int stride,
		       int source_width, int source_height,
		       const struct flow_overlay *overlay, double preview_fps, double pts_ms)
{
	(void)flow_draw_overlay_rotated(screen, width, height, stride, 0,
		source_width, source_height, overlay, preview_fps, pts_ms);
}

int flow_draw_overlay_rotated(uint8_t *screen, int width, int height, int stride,
		       int screen_rotation, int source_width, int source_height,
		       const struct flow_overlay *overlay, double preview_fps, double pts_ms)
{
	if (!screen || !overlay || width <= 0 || width > 8192 || height <= 0 || height > 8192 ||
	    stride < width * 4 || source_width <= 0 || source_height <= 0 ||
	    (screen_rotation != 0 && screen_rotation != 90 && screen_rotation != 180 && screen_rotation != 270))
		return -1;
	struct drawing_surface surface = drawing_surface(screen, width, height, stride, screen_rotation);
	width = surface.width;
	height = surface.height;
	const char *zone_names[] = {"IN", "TRANS", "OUT"};
	const uint32_t colors[] = {0x40ff40U, 0xffdf40U, 0xff7040U};
	double scale = fmin((double)width / source_width, (double)height / source_height);
	int ow = (int)lrint(source_width * scale), oh = (int)lrint(source_height * scale);
	int left = (width - ow) / 2, top = (height - oh) / 2;
	double age = overlay->pts_ms > 0 ? pts_ms - overlay->pts_ms : -1;
	char label[128];
	if (overlay->enabled) {
		for (int z = 0; z < 3; ++z) {
			const struct flow_polygon *polygon = overlay->zones + z;
			double cx = 0, cy = 0;
			for (size_t i = 0; i < polygon->count; ++i) {
				size_t next = (i + 1) % polygon->count;
				int x0 = left + (int)lrint(polygon->xy[i][0] * (ow - 1));
				int y0 = top + (int)lrint(polygon->xy[i][1] * (oh - 1));
				int x1 = left + (int)lrint(polygon->xy[next][0] * (ow - 1));
				int y1 = top + (int)lrint(polygon->xy[next][1] * (oh - 1));
				line(&surface, x0, y0, x1, y1, colors[z]);
				cx += polygon->xy[i][0]; cy += polygon->xy[i][1];
			}
			if (polygon->count) text(&surface,
				left + (int)(cx / polygon->count * ow) - 18,
				top + (int)(cy / polygon->count * oh), zone_names[z], colors[z]);
		}
	}
	/* Boxes explicitly belong to the latest AI frame; never retain arbitrarily old boxes. */
	if (age >= 0 && age <= 400)
		for (size_t i = 0; i < overlay->count; ++i) {
			const struct flow_track_view *view = overlay->tracks + i;
			const struct flow_detection *d = &view->detection;
			uint32_t color = d->class_id == 0 ? 0x40ff40U : 0x40dfffU;
			int x1 = left + (int)lrint(d->xyxy[0] * (ow - 1));
			int y1 = top + (int)lrint(d->xyxy[1] * (oh - 1));
			int x2 = left + (int)lrint(d->xyxy[2] * (ow - 1));
			int y2 = top + (int)lrint(d->xyxy[3] * (oh - 1));
			int ly = y1 >= 18 ? y1 - 18 : y1 + 3;
			line(&surface, x1, y1, x2, y1, color);
			line(&surface, x2, y1, x2, y2, color);
			line(&surface, x2, y2, x1, y2, color);
			line(&surface, x1, y2, x1, y1, color);
			rectangle(&surface, (x1 + x2) / 2 - 2, (y1 + y2) / 2 - 2, 5, 5, color);
			if (view->track_id >= 0)
				snprintf(label, sizeof(label), "%s #%d %.2f %s%s", d->class_id == 0 ? "H" : "HEAD",
					view->track_id, d->score, view->zone ? zone_names[view->zone - 1] : "?",
					view->pending ? " PENDING" : "");
			else snprintf(label, sizeof(label), "%s %.2f UNTRACKED", d->class_id == 0 ? "H" : "HEAD", d->score);
			rectangle(&surface, x1, ly, (int)strlen(label) * 12, 18, 0);
			text(&surface, x1, ly + 2, label, color);
		}
	rectangle(&surface, 0, 0, width, 78, 0);
	snprintf(label, sizeof(label), "%s TAKE %d PUT %d", !overlay->enabled ? "NO ZONES" :
		overlay->geometry_confirmed ? "LIVE" : "DEBUG", overlay->take_out, overlay->put_in);
	text(&surface, 6, 3, label, overlay->geometry_confirmed ? 0xffffffU : 0xffdf40U);
	snprintf(label, sizeof(label), "UNCERTAIN %d PENDING %d", overlay->uncertain, overlay->pending);
	text(&surface, 6, 21, label, 0xffffffU);
	snprintf(label, sizeof(label), "VIEW %.1f AI %.1f FPS", preview_fps, overlay->detector_fps);
	text(&surface, 6, 39, label, 0xffffffU);
	snprintf(label, sizeof(label), "NPU %.0f MS AI AGE %.0f MS", overlay->infer_ms, age);
	text(&surface, 6, 57, label, age > 400 ? 0xff7040U : 0xffffffU);
	rectangle(&surface, 0, height - 60, width, 60, 0);
	text(&surface, 6, height - 57, overlay->last_event, 0xffdf40U);
	/* Two short rows preserve the complete reason on a 480px-wide screen. */
	char reason[81] = {0};
	for (size_t i = 0; i < sizeof(reason) - 1 && overlay->reason[i]; ++i)
		reason[i] = overlay->reason[i] == '_' ? ' ' : (char)toupper((unsigned char)overlay->reason[i]);
	char second[41] = {0};
	if (strlen(reason) > 38) { snprintf(second, sizeof(second), "%.38s", reason + 38); reason[38] = '\0'; }
	text(&surface, 6, height - 38, reason, 0xffffffU);
	text(&surface, 6, height - 20, second, 0xffffffU);
	return 0;
}
