#include "bar_scene.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

enum {
	BAR_WIDTH = 32,
	STEP = 8,
	// Where the bar can start, 8 pixels apart; it leaves at the right edge and comes back at the left.
	POSITIONS = BAR_FRAME_WIDTH / STEP,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	COLOR_OFFSET_BYTES = 16,
	CLEAR_ARGB = 0xFF202020,
	// A pixel row the checks look at, and a column half the frame away from the bar.
	CHECK_Y = BAR_FRAME_HEIGHT / 2,
	FAR_AWAY = BAR_FRAME_WIDTH / 2,
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static uint32_t bar_x(uint32_t frame)
{
	return frame % POSITIONS * STEP;
}

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

// The bar at each position, two triangles a position, edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
static volatile uint32_t *upload_bars(uint32_t argb)
{
	static float bars[POSITIONS][QUAD_VERTICES][VERTEX_FLOATS];
	const float red = (argb >> 16 & 0xFF) / 255.0f;
	const float green = (argb >> 8 & 0xFF) / 255.0f;
	const float blue = (argb & 0xFF) / 255.0f;
	const float top = 1.0f + 0.5f / (BAR_FRAME_HEIGHT / 2.0f);
	const float bottom = 1.0f - (BAR_FRAME_HEIGHT - 0.5f) / (BAR_FRAME_HEIGHT / 2.0f);
	for (uint32_t i = 0; i < POSITIONS; i++) {
		const float left = (bar_x(i) - 0.5f) / (BAR_FRAME_WIDTH / 2.0f) - 1.0f;
		const float right = (bar_x(i) + BAR_WIDTH - 0.5f) / (BAR_FRAME_WIDTH / 2.0f) - 1.0f;
		const float corners[QUAD_VERTICES][2] = { { left, top }, { right, top }, { left, bottom }, { left, bottom },
			{ right, top }, { right, bottom } };
		for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
			const float vertex[VERTEX_FLOATS] = { corners[v][0], corners[v][1], 0.0f, 1.0f, red, green, blue, 1.0f };
			memcpy(bars[i][v], vertex, sizeof vertex);
		}
	}
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof bars);
	if (!vertices)
		return NULL;
	uint32_t word;
	for (uint32_t i = 0; i < sizeof bars / 4; i++) {
		memcpy(&word, (const uint8_t *)bars + i * 4, 4);
		vertices[i] = word;
	}
	__asm__ volatile("sync" ::: "memory");
	return vertices;
}

bool bar_scene_open(BarScene *scene, uint32_t argb)
{
	*scene = (BarScene){ .vertex = { .microcode = NULL }, .pixel = { .microcode = NULL }, .argb = argb };
	if (!NT_SUCCESS(xsdk_shader_load(&scene->vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&scene->pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&scene->vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	scene->vertices = upload_bars(argb);
	return scene->vertices != NULL;
}

void bar_scene_draw(const BarScene *scene, uint32_t frame, uint32_t address)
{
	xsdk_gpu_set_target(BAR_FRAME_WIDTH, BAR_FRAME_HEIGHT);
	xsdk_gpu_set_shaders(&scene->vertex, &scene->pixel);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(scene->vertices) + frame % POSITIONS * QUAD_BYTES, QUAD_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	xsdk_gpu_resolve(address, BAR_FRAME_WIDTH, BAR_FRAME_HEIGHT, CLEAR_ARGB);
}

bool bar_scene_holds(const BarScene *scene, volatile const uint32_t *pixels, uint32_t frame)
{
	const uint32_t x = bar_x(frame);
	return pixels[tiled_index(x + 4, CHECK_Y, BAR_FRAME_WIDTH)] == as_stored(scene->argb) &&
		pixels[tiled_index((x + FAR_AWAY) % BAR_FRAME_WIDTH, CHECK_Y, BAR_FRAME_WIDTH)] == as_stored(CLEAR_ARGB);
}

void bar_scene_log_miss(const BarScene *scene, volatile const uint32_t *pixels, uint32_t frame, const char *name)
{
	const uint32_t x = bar_x(frame);
	xsdk_log_write("%s: frame %u: bar pixel (%u, %u) held %08X, not %08X; pixel (%u, %u) held %08X, not %08X", name,
		(unsigned)frame, (unsigned)(x + 4), (unsigned)CHECK_Y,
		(unsigned)pixels[tiled_index(x + 4, CHECK_Y, BAR_FRAME_WIDTH)], (unsigned)as_stored(scene->argb),
		(unsigned)((x + FAR_AWAY) % BAR_FRAME_WIDTH), (unsigned)CHECK_Y,
		(unsigned)pixels[tiled_index((x + FAR_AWAY) % BAR_FRAME_WIDTH, CHECK_Y, BAR_FRAME_WIDTH)],
		(unsigned)as_stored(CLEAR_ARGB));
}

void bar_scene_close(BarScene *scene)
{
	xsdk_gpu_free(scene->vertices);
	xsdk_shader_free(&scene->pixel);
	xsdk_shader_free(&scene->vertex);
	*scene = (BarScene){ .vertices = NULL };
}
