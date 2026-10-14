#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constant_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// One rectangle drawn eight ways from index buffers, each in its own cell and colour.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 6,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CELL_WIDTH = 240,
	CELL_HEIGHT = 200,
	// The rectangle's corners and centre, stored out of order so only the indices put them right.
	BOTTOM_RIGHT = 0,
	TOP_LEFT = 1,
	CENTRE = 2,
	BOTTOM_LEFT = 3,
	TOP_RIGHT = 4,
	CORNERS = 5,
	// A copy of the corners past what 16 bits can index; its 16-bit truncation lands on degenerate vertices.
	HIGH_BASE = 0x10008,
	TRUNCATED_BASE = HIGH_BASE & 0xFFFF,
	VERTICES = HIGH_BASE + CORNERS,
	INDEX_BUFFER_BYTES = 256,
	// Inside a filled rectangle, points this far in from its corners; outside, this far out.
	FILL_MARGIN = 3,
	// A line may fall on the rows or columns either side of the edge it traces, a point anywhere this close.
	EDGE_SLACK = 2,
	POINT_SLACK = 2,
	CHANNEL_TOLERANCE = 1,
};

typedef enum {
	FILLED,
	OUTLINE,
	POINTS,
} Shape;

typedef struct {
	const char *name;
	uint32_t primitive_type;
	bool indices_32_bit;
	uint32_t index_count;
	uint32_t index_base;
	uint32_t indices[8];
	Shape shape;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	// Set at start: where the list sits in the index buffer.
	uint32_t offset_bytes;
} Way;

static Way ways[] = {
	{ "triangle list, 16-bit", XSDK_PRIMITIVE_TRIANGLE_LIST, false, 6, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_LEFT, TOP_RIGHT, BOTTOM_RIGHT }, FILLED, 80, 100, 0xFFE04040, 0 },
	{ "triangle list, 32-bit", XSDK_PRIMITIVE_TRIANGLE_LIST, true, 6, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_LEFT, TOP_RIGHT, BOTTOM_RIGHT }, FILLED, 380, 100, 0xFF40E040, 0 },
	{ "triangle strip, 16-bit", XSDK_PRIMITIVE_TRIANGLE_STRIP, false, 4, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT }, FILLED, 680, 100, 0xFF4040E0, 0 },
	{ "triangle strip, 32-bit", XSDK_PRIMITIVE_TRIANGLE_STRIP, true, 4, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT }, FILLED, 980, 100, 0xFFE0E040, 0 },
	{ "line list, 16-bit", XSDK_PRIMITIVE_LINE_LIST, false, 8, 0,
		{ TOP_LEFT, TOP_RIGHT, TOP_RIGHT, BOTTOM_RIGHT, BOTTOM_RIGHT, BOTTOM_LEFT, BOTTOM_LEFT, TOP_LEFT }, OUTLINE, 80,
		420, 0xFFE040E0, 0 },
	{ "line strip, 32-bit", XSDK_PRIMITIVE_LINE_STRIP, true, 5, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_RIGHT, BOTTOM_LEFT, TOP_LEFT }, OUTLINE, 380, 420, 0xFF40E0E0, 0 },
	{ "point list, 16-bit", XSDK_PRIMITIVE_POINT_LIST, false, 5, 0,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_RIGHT, CENTRE }, POINTS, 680, 420, 0xFFFFFFFF, 0 },
	{ "triangle list, 32-bit, indices past 65535", XSDK_PRIMITIVE_TRIANGLE_LIST, true, 6, HIGH_BASE,
		{ TOP_LEFT, TOP_RIGHT, BOTTOM_LEFT, BOTTOM_LEFT, TOP_RIGHT, BOTTOM_RIGHT }, FILLED, 980, 420, 0xFFFF8000, 0 },
};

// Unit-square coordinates for each stored vertex.
static const float corner_uv[CORNERS][2] = {
	[BOTTOM_RIGHT] = { 1.0f, 1.0f },
	[TOP_LEFT] = { 0.0f, 0.0f },
	[CENTRE] = { 0.5f, 0.5f },
	[BOTTOM_LEFT] = { 0.0f, 1.0f },
	[TOP_RIGHT] = { 1.0f, 0.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static void put_vertex(volatile uint32_t *vertices, uint32_t index, float u, float v)
{
	vertices[index * VERTEX_FLOATS] = float_bits(u);
	vertices[index * VERTEX_FLOATS + 1] = float_bits(v);
	vertices[index * VERTEX_FLOATS + 2] = 0;
	vertices[index * VERTEX_FLOATS + 3] = float_bits(1.0f);
}

// The corners at 0 and at HIGH_BASE, and degenerate vertices where HIGH_BASE's indices cut to 16 bits would land.
static volatile uint32_t *upload_vertices(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(VERTICES * STRIDE_BYTES);
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < CORNERS; i++) {
		put_vertex(vertices, i, corner_uv[i][0], corner_uv[i][1]);
		put_vertex(vertices, HIGH_BASE + i, corner_uv[i][0], corner_uv[i][1]);
		put_vertex(vertices, TRUNCATED_BASE + i, 0.0f, 0.0f);
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), VERTICES * STRIDE_BYTES);
	return vertices;
}

// Every list one after another, big-endian as the CPU stores them, each starting on 4 bytes.
static volatile uint32_t *upload_indices(void)
{
	static union {
		uint8_t bytes[INDEX_BUFFER_BYTES];
		uint32_t words[INDEX_BUFFER_BYTES / 4];
	} buffer;
	uint32_t offset = 0;
	for (uint32_t w = 0; w < sizeof ways / sizeof ways[0]; w++) {
		Way *const way = &ways[w];
		way->offset_bytes = offset;
		for (uint32_t i = 0; i < way->index_count; i++) {
			const uint32_t index = way->index_base + way->indices[i];
			if (way->indices_32_bit) {
				memcpy(&buffer.bytes[offset], &index, 4);
				offset += 4;
			} else {
				const uint16_t short_index = (uint16_t)index;
				memcpy(&buffer.bytes[offset], &short_index, 2);
				offset += 2;
			}
		}
		offset = (offset + 3) & ~3u;
	}
	volatile uint32_t *const indices = xsdk_gpu_alloc(INDEX_BUFFER_BYTES);
	if (!indices)
		return NULL;
	for (uint32_t i = 0; i < offset / 4; i++)
		indices[i] = buffer.words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(indices), INDEX_BUFFER_BYTES);
	return indices;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square over the cell, edges half a pixel early, and colours it with c2.
static void set_way_constants(const Way *way)
{
	const float left = (way->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (way->x + CELL_WIDTH - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (way->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (way->y + CELL_HEIGHT - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, 0.0f, 0.0f },
		{ unorm(way->argb >> 16), unorm(way->argb >> 8), unorm(way->argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
}

static bool matches(volatile const uint32_t *pixels, uint32_t x, uint32_t y, uint32_t argb)
{
	const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

// Whether any pixel in the box around (x, y) holds the colour.
static bool near(
	volatile const uint32_t *pixels, uint32_t x, uint32_t y, uint32_t slack_x, uint32_t slack_y, uint32_t argb)
{
	for (uint32_t py = y - slack_y; py <= y + slack_y; py++)
		for (uint32_t px = x - slack_x; px <= x + slack_x; px++)
			if (matches(pixels, px, py, argb))
				return true;
	return false;
}

static bool expect(bool holds, const Way *way, uint32_t frame, const char *what, uint32_t x, uint32_t y,
	volatile const uint32_t *pixels, uint32_t *misses)
{
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("indexed: frame %u: %s: %s at (%u, %u), which holds %08X", (unsigned)frame, way->name, what,
			(unsigned)x, (unsigned)y, (unsigned)pixels[tiled_index(x, y, WIDTH)]);
	*misses += !holds;
	return holds;
}

static bool way_holds(volatile const uint32_t *pixels, const Way *way, uint32_t frame, uint32_t *misses)
{
	const uint32_t left = way->x;
	const uint32_t top = way->y;
	const uint32_t right = way->x + CELL_WIDTH - 1;
	const uint32_t bottom = way->y + CELL_HEIGHT - 1;
	const uint32_t mid_x = way->x + CELL_WIDTH / 2;
	const uint32_t mid_y = way->y + CELL_HEIGHT / 2;
	bool holds = true;
	if (way->shape == FILLED) {
		const uint32_t inside[5][2] = { { mid_x, mid_y }, { left + FILL_MARGIN, top + FILL_MARGIN },
			{ right - FILL_MARGIN, top + FILL_MARGIN }, { left + FILL_MARGIN, bottom - FILL_MARGIN },
			{ right - FILL_MARGIN, bottom - FILL_MARGIN } };
		for (uint32_t i = 0; i < 5; i++)
			holds &= expect(matches(pixels, inside[i][0], inside[i][1], way->argb), way, frame, "the fill missing",
				inside[i][0], inside[i][1], pixels, misses);
		holds &= expect(matches(pixels, left - FILL_MARGIN, mid_y, CLEAR_ARGB), way, frame, "drawn past the left",
			left - FILL_MARGIN, mid_y, pixels, misses);
		holds &= expect(matches(pixels, mid_x, top - FILL_MARGIN, CLEAR_ARGB), way, frame, "drawn past the top", mid_x,
			top - FILL_MARGIN, pixels, misses);
	} else if (way->shape == OUTLINE) {
		holds &= expect(
			near(pixels, mid_x, top, 0, EDGE_SLACK, way->argb), way, frame, "no top edge", mid_x, top, pixels, misses);
		holds &= expect(near(pixels, mid_x, bottom, 0, EDGE_SLACK, way->argb), way, frame, "no bottom edge", mid_x,
			bottom, pixels, misses);
		holds &= expect(near(pixels, left, mid_y, EDGE_SLACK, 0, way->argb), way, frame, "no left edge", left, mid_y,
			pixels, misses);
		holds &= expect(near(pixels, right, mid_y, EDGE_SLACK, 0, way->argb), way, frame, "no right edge", right, mid_y,
			pixels, misses);
		holds &= expect(
			matches(pixels, mid_x, mid_y, CLEAR_ARGB), way, frame, "filled inside", mid_x, mid_y, pixels, misses);
	} else {
		const uint32_t points[5][2] = { { left, top }, { right, top }, { left, bottom }, { right, bottom },
			{ mid_x, mid_y } };
		for (uint32_t i = 0; i < 5; i++)
			holds &= expect(near(pixels, points[i][0], points[i][1], POINT_SLACK, POINT_SLACK, way->argb), way, frame,
				"a point missing", points[i][0], points[i][1], pixels, misses);
		// Nothing on the top edge between the corners, across the rows a line along it would take.
		holds &= expect(!near(pixels, mid_x, top, 0, EDGE_SLACK, way->argb), way, frame, "drawn between points", mid_x,
			top, pixels, misses);
		holds &= expect(matches(pixels, (left + mid_x) / 2, (top + mid_y) / 2, CLEAR_ARGB), way, frame,
			"drawn between points", (left + mid_x) / 2, (top + mid_y) / 2, pixels, misses);
	}
	return holds;
}

// Each frame draws every way into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices, uint32_t indices,
	volatile uint32_t *const *buffers, bool has_display)
{
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(0, vertices, VERTICES * STRIDE_BYTES);
		for (uint32_t w = 0; w < sizeof ways / sizeof ways[0]; w++) {
			set_way_constants(&ways[w]);
			xsdk_gpu_draw_indexed(
				ways[w].primitive_type, indices + ways[w].offset_bytes, ways[w].index_count, ways[w].indices_32_bit);
		}
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("indexed: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed) {
			bool holds = true;
			for (uint32_t w = 0; w < sizeof ways / sizeof ways[0]; w++)
				holds &= way_holds(buffer, &ways[w], frame, &misses);
			frames_wrong += !holds;
		}
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("indexed: %u of %u frames checked held a wrong point; %u points wrong", (unsigned)frames_wrong,
		(unsigned)(FRAMES - 1), (unsigned)misses);
	check("indexed: every frame's fence landed", fences_landed);
	check("indexed: every way drew its shape in its cell", fences_landed && frames_wrong == 0);
	if (has_display) {
		check("indexed: every frame shown", shown);
		xsdk_log_write("indexed: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

void test_indexed(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("indexed: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("indexed: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("indexed: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("indexed: vertex fetch patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_vertices();
	volatile uint32_t *const indices = upload_indices();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("indexed: vertex, index and front buffers", vertices && indices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && indices && buffers[0] && buffers[1])
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), xsdk_gpu_address(indices), buffers, has_display);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(indices);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("indexed: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
