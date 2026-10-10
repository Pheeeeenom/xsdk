#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	BUFFERS = 2,
	FRAMES = 120,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	// The corners land at (320, 60), (80, 420) and (560, 420): half of 480 by 360 pixels, give or take its edges.
	TRIANGLE_PIXELS = 480 * 360 / 2,
	TRIANGLE_TOLERANCE_PIXELS = 2000,
	// Near a corner its colour is above this, and the other two below the low mark.
	CHANNEL_HIGH = 0xC0,
	CHANNEL_LOW = 0x40,
};

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	// Which byte of a stored word should be bright: 8 is red, 16 green, 24 blue, as the display reads them.
	uint32_t shift;
} Corner;

// Position in clip space, then colour: red at the top, green bottom left, blue bottom right.
static const float triangle[3][VERTEX_FLOATS] = {
	{ 0.0f, 0.75f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
	{ -0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

// A pixel just inside each corner.
static const Corner corners[] = {
	{ "red", 320, 80, 8 },
	{ "green", 100, 410, 16 },
	{ "blue", 540, 410, 24 },
};

static const uint32_t outside_x = 20;
static const uint32_t outside_y = 20;

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

static bool corner_holds(volatile const uint32_t *pixels, const Corner *corner)
{
	const uint32_t pixel = pixels[tiled_index(corner->x, corner->y, WIDTH)];
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t channel = pixel >> shift & 0xFF;
		if (shift == corner->shift ? channel < CHANNEL_HIGH : channel > CHANNEL_LOW)
			return false;
	}
	return true;
}

// The corners hold their colours, and a pixel outside the triangle the clear colour.
static bool frame_holds(volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses)
{
	bool holds = pixels[tiled_index(outside_x, outside_y, WIDTH)] == as_stored(CLEAR_ARGB);
	for (uint32_t i = 0; i < sizeof corners / sizeof corners[0]; i++) {
		if (corner_holds(pixels, &corners[i]))
			continue;
		if (*misses < MAX_MISSES_LOGGED)
			xsdk_log_write("triangle: frame %u: the %s corner held %08X", (unsigned)frame, corners[i].name,
				(unsigned)pixels[tiled_index(corners[i].x, corners[i].y, WIDTH)]);
		holds = false;
	}
	*misses += !holds;
	return holds;
}

static uint32_t count_drawn(volatile const uint32_t *pixels)
{
	uint32_t drawn = 0;
	for (uint32_t i = 0; i < PIXELS; i++)
		drawn += pixels[i] != as_stored(CLEAR_ARGB);
	return drawn;
}

static volatile uint32_t *upload_triangle(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof triangle);
	if (!vertices)
		return NULL;
	uint32_t words[sizeof triangle / 4];
	memcpy(words, triangle, sizeof triangle);
	for (uint32_t i = 0; i < sizeof triangle / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	return vertices;
}

// Each frame draws the triangle into eDRAM, resolves it into the buffer not on screen, clearing eDRAM, and shows it.
static void draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *const *buffers, bool has_display)
{
	uint32_t misses = 0;
	bool fences_landed = true;
	bool shown = true;
	uint32_t drawn = 0;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(vertices, sizeof triangle);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed)
			(void)frame_holds(buffer, frame, &misses);
		if (frame == FRAMES - 1 && fences_landed)
			drawn = count_drawn(buffer);
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("triangle: the last frame drew %u pixels", (unsigned)drawn);
	check("triangle: every frame's fence landed", fences_landed);
	check("triangle: every frame has red, green and blue corners on the clear colour", fences_landed && misses == 0);
	check("triangle: the last frame covers the triangle's area",
		drawn + TRIANGLE_TOLERANCE_PIXELS >= TRIANGLE_PIXELS && drawn <= TRIANGLE_PIXELS + TRIANGLE_TOLERANCE_PIXELS);
	if (has_display)
		check("triangle: every frame shown", shown);
}

void test_triangle(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("triangle: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display = NT_SUCCESS(xsdk_display_open(&width, &height)) && width == WIDTH && height == HEIGHT;
	check("triangle: display open in a 640x480 mode", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("triangle: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, sizeof layout / sizeof layout[0], STRIDE_BYTES));
	check("triangle: vertex fetches patched for the layout", patched);
	volatile uint32_t *const vertices = upload_triangle();
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(PIXELS * 4), xsdk_gpu_alloc(PIXELS * 4) };
	check("triangle: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1])
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, has_display);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("triangle: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
