#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "hiz_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

static void check(const char *name, bool passed)
{
	xsdk_log_write("%s: %s", passed ? "PASS" : "FAIL", name);
}

// A copy of tests/test_hiz.c as a probe, timing each frame's first rectangle through the long shader and logging
// each frame's wrong pixels, to see whether the buffer rejects in the frames.
// hidden pixels through a long pixel shader are timed against drawn ones,
// and frames of near and far rectangles, drawn both ways over depth clears, less-than and greater-than, are checked
// pixel by pixel against the CPU's depth test. Each frame starts by drawing far over what the frame before left near,
// so a hierarchical buffer left uncleared when depth clears hides it.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_US = 2000000,
	MAX_MISSES_LOGGED = 6,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	TIMED_QUADS = 8,
	// Hidden pixels must cost under an eighth of drawn ones; without the buffer they cost the same.
	HIDDEN_SHARE = 8,
	FRAMES = 12,
	RANDOM_RECTANGLES = 24,
	MAX_RECTANGLES = RANDOM_RECTANGLES + 8,
	DEPTH_24_MAX = 0xFFFFFF,
	// Colours by rectangle: the clear, then one each.
	CLEARED = 0,
};

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	float depth;
	uint32_t compare;
} Rectangle;

typedef struct {
	float clear_depth;
	// Greater-than draws, else less-than.
	bool greater;
	// A full-screen write with always partway through.
	bool always;
} Frame;

static const Frame frames[FRAMES] = {
	{ 1.0f, false, false },
	{ 1.0f, false, false },
	{ 1.0f, false, true },
	{ 1.0f, false, false },
	{ 0.0f, true, false },
	{ 0.0f, true, false },
	{ 0.0f, true, true },
	{ 0.0f, true, false },
	{ 1.0f, false, false },
	{ 0.75f, false, false },
	{ 0.0f, true, false },
	{ 1.0f, false, false },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static uint32_t model_depth[PIXELS];
static uint8_t model_colour[PIXELS];
static XsdkDepthStencilState depth_states[XSDK_COMPARE_ALWAYS + 1];
static uint32_t random_state;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

// Spins on a new fence, so the time is the GPU's to within a few microseconds; 0 if it didn't land.
static uint64_t fence_time_us(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US)
			return 0;
	}
	return now_us();
}

static uint32_t random_below(uint32_t limit)
{
	random_state = random_state * 1103515245 + 12345;
	return (random_state >> 8) % limit;
}

static uint32_t colour_of(uint32_t index)
{
	if (index == CLEARED)
		return CLEAR_ARGB;
	// Up to 32 colours, red and green 32 steps apart, none with the clear's red.
	return 0xFF0000C0 | ((index & 7) * 32 + 16) << 16 | ((index >> 3 & 3) * 64 + 32) << 8;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square, edges half a pixel early, at the depth in c1.z, coloured by c2.
static void draw_rectangle(const Rectangle *r, uint32_t argb, uint32_t count)
{
	const float left = (r->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (r->x + r->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (r->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (r->y + r->height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, r->depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static uint32_t depth24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

static bool passes(uint32_t compare, uint32_t incoming, uint32_t stored)
{
	switch (compare) {
	case XSDK_COMPARE_LESS:
		return incoming < stored;
	case XSDK_COMPARE_LESS_EQUAL:
		return incoming <= stored;
	case XSDK_COMPARE_GREATER:
		return incoming > stored;
	case XSDK_COMPARE_GREATER_EQUAL:
		return incoming >= stored;
	default:
		return compare == XSDK_COMPARE_ALWAYS;
	}
}

static void model_draw(const Rectangle *r, uint32_t colour)
{
	const uint32_t z = depth24(r->depth);
	for (uint32_t y = r->y; y < r->y + r->height; y++) {
		for (uint32_t x = r->x; x < r->x + r->width; x++) {
			const uint32_t i = y * WIDTH + x;
			if (passes(r->compare, z, model_depth[i])) {
				model_depth[i] = z;
				model_colour[i] = (uint8_t)colour;
			}
		}
	}
}

static void model_clear(float depth)
{
	for (uint32_t i = 0; i < PIXELS; i++) {
		model_depth[i] = depth24(depth);
		model_colour[i] = CLEARED;
	}
}

// From 0.1 to 0.9, a share of 1 nearest: low depths for less-than, high ones for greater-than.
static float frame_depth(const Frame *f, float near_share)
{
	const float depth = 0.1f + 0.8f * (1.0f - near_share);
	return f->greater ? 1.0f - depth : depth;
}

// Far over the frame before's near first, pairs drawn both ways, random rectangles, maybe an always write, then near
// over the whole target last, for the frame after.
static uint32_t make_rectangles(const Frame *f, Rectangle *out)
{
	const uint32_t less = f->greater ? XSDK_COMPARE_GREATER : XSDK_COMPARE_LESS;
	const uint32_t less_equal = f->greater ? XSDK_COMPARE_GREATER_EQUAL : XSDK_COMPARE_LESS_EQUAL;
	uint32_t n = 0;
	out[n++] = (Rectangle){ 0, 0, WIDTH, HEIGHT, frame_depth(f, 0.25f), less };
	// Back to front, then front to back, overlapping.
	out[n++] = (Rectangle){ 40, 40, 300, 200, frame_depth(f, 0.5f), less };
	out[n++] = (Rectangle){ 140, 140, 300, 200, frame_depth(f, 0.75f), less_equal };
	out[n++] = (Rectangle){ 540, 40, 300, 200, frame_depth(f, 0.75f), less };
	out[n++] = (Rectangle){ 640, 140, 300, 200, frame_depth(f, 0.5f), less_equal };
	for (uint32_t i = 0; i < RANDOM_RECTANGLES; i++) {
		const uint32_t width = 1 + random_below(400);
		const uint32_t height = 1 + random_below(300);
		const uint32_t x = random_below(WIDTH - width + 1);
		const uint32_t y = random_below(HEIGHT - height + 1);
		const float near_share = 0.3f + random_below(600) / 1000.0f;
		out[n++] = (Rectangle){ x, y, width, height, frame_depth(f, near_share), i % 3 ? less : less_equal };
		if (f->always && i == RANDOM_RECTANGLES / 2)
			out[n++] = (Rectangle){ 0, 0, WIDTH, HEIGHT, frame_depth(f, 0.5f), XSDK_COMPARE_ALWAYS };
	}
	out[n++] = (Rectangle){ 0, 0, WIDTH, HEIGHT, frame_depth(f, 1.0f), less };
	return n;
}

static void begin_target(uint32_t vertices)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices, sizeof square);
}

static bool matches(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static uint32_t wrong_pixels(volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses_logged)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
			const uint32_t argb = colour_of(model_colour[y * WIDTH + x]);
			if (matches(stored, argb))
				continue;
			if (*misses_logged < MAX_MISSES_LOGGED) {
				xsdk_log_write("hiz: frame %u: (%u, %u) held %08X, not %08X", (unsigned)frame, (unsigned)x, (unsigned)y,
					(unsigned)stored, (unsigned)__builtin_bswap32(argb));
				(*misses_logged)++;
			}
			wrong++;
		}
	}
	return wrong;
}

// Each frame's resolve clears depth to the next frame's clear; the first one's colour and depth are eDRAM's.
static void check_frames(const XsdkShader *vertex, const XsdkShader *pixel, const XsdkShader *long_pixel,
	uint32_t vertices, volatile uint32_t *buffer)
{
	static Rectangle rectangles[MAX_RECTANGLES];
	random_state = 30;
	uint32_t frames_wrong = 0;
	uint32_t pixels_wrong = 0;
	uint32_t misses_logged = 0;
	bool landed = true;
	const float zero[4] = { 0, 0, 0, 0 };
	xsdk_gpu_set_depth_clear(frames[0].clear_depth, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	for (uint32_t frame = 0; frame < FRAMES && landed; frame++) {
		const Frame *const f = &frames[frame];
		begin_target(vertices);
		xsdk_gpu_set_pixel_constants(0, zero, 1);
		xsdk_gpu_set_shaders(vertex, pixel);
		model_clear(f->clear_depth);
		const uint32_t count = make_rectangles(f, rectangles);
		uint64_t first_us = 0;
		for (uint32_t i = 0; i < count; i++) {
			xsdk_gpu_set_depth_stencil(&depth_states[rectangles[i].compare]);
			if (i == 0) {
				xsdk_gpu_set_shaders(vertex, long_pixel);
				const uint64_t start = fence_time_us();
				draw_rectangle(&rectangles[i], colour_of(i + 1), 1);
				first_us = fence_time_us() - start;
				xsdk_gpu_set_shaders(vertex, pixel);
			} else {
				draw_rectangle(&rectangles[i], colour_of(i + 1), 1);
			}
			model_draw(&rectangles[i], i + 1);
		}
		xsdk_gpu_set_depth_clear(frames[(frame + 1) % FRAMES].clear_depth, 0);
		xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
		landed = fence_time_us() != 0;
		if (!landed)
			break;
		const uint32_t wrong = wrong_pixels(buffer, frame, &misses_logged);
		xsdk_log_write("hiz frames probe: frame %u: first rectangle %u us, %u pixels wrong", (unsigned)frame,
			(unsigned)first_us, (unsigned)wrong);
		frames_wrong += wrong != 0;
		pixels_wrong += wrong;
	}
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_log_write("hiz: %u of %u frames held a wrong pixel, %u pixels in all", (unsigned)frames_wrong,
		(unsigned)FRAMES, (unsigned)pixels_wrong);
	check("hiz: every frame's fence landed", landed);
	check("hiz: near and far drawn both ways over depth clears, less-than and greater-than, every pixel right",
		landed && frames_wrong == 0);
}

// Over a clear to 1: a near quad, then quads through the long shader behind it, then the same drawn over it.
static void check_hidden_time(const XsdkShader *vertex, const XsdkShader *short_pixel, const XsdkShader *long_pixel,
	uint32_t vertices, volatile uint32_t *buffer)
{
	const Rectangle near = { 0, 0, WIDTH, HEIGHT, 0.25f, XSDK_COMPARE_LESS };
	const Rectangle hidden = { 0, 0, WIDTH, HEIGHT, 0.75f, XSDK_COMPARE_LESS };
	const Rectangle drawn = { 0, 0, WIDTH, HEIGHT, 0.1f, XSDK_COMPARE_ALWAYS };
	const float zero[4] = { 0, 0, 0, 0 };
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	begin_target(vertices);
	xsdk_gpu_set_pixel_constants(0, zero, 1);
	xsdk_gpu_set_depth_stencil(&depth_states[XSDK_COMPARE_LESS]);
	xsdk_gpu_set_shaders(vertex, short_pixel);
	draw_rectangle(&near, colour_of(1), 1);
	xsdk_gpu_set_shaders(vertex, long_pixel);
	const uint64_t start = fence_time_us();
	draw_rectangle(&hidden, colour_of(2), TIMED_QUADS);
	const uint64_t middle = fence_time_us();
	xsdk_gpu_set_depth_stencil(&depth_states[XSDK_COMPARE_ALWAYS]);
	draw_rectangle(&drawn, colour_of(3), TIMED_QUADS);
	const uint64_t end = fence_time_us();
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	const bool landed = start && middle && end && fence_time_us();
	const uint32_t hidden_us = (uint32_t)(middle - start);
	const uint32_t drawn_us = (uint32_t)(end - middle);
	if (landed)
		xsdk_log_write("hiz: %u hidden full-screen quads through the long shader took %u us, %u drawn ones %u us",
			(unsigned)TIMED_QUADS, (unsigned)hidden_us, (unsigned)TIMED_QUADS, (unsigned)drawn_us);
	check("hiz: hidden pixels through a long shader cost under an eighth of drawn ones",
		landed && hidden_us * HIDDEN_SHARE < drawn_us);
}

static volatile uint32_t *upload_square(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	if (!vertices)
		return NULL;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), sizeof square);
	return vertices;
}

int main(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("hiz: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return 1;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader short_pixel = { .microcode = NULL };
	XsdkShader long_pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&short_pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&long_pixel, long_pixel_shader, sizeof long_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("hiz: shaders load", loaded);
	for (uint32_t compare = 0; compare <= XSDK_COMPARE_ALWAYS; compare++) {
		const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = compare };
		xsdk_gpu_depth_stencil_state(&depth_states[compare], &setting);
	}
	volatile uint32_t *const vertices = upload_square();
	volatile uint32_t *const buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	check("hiz: vertex and resolve buffers", vertices && buffer);
	if (loaded && vertices && buffer) {
		check_hidden_time(&vertex, &short_pixel, &long_pixel, xsdk_gpu_address(vertices), buffer);
		check_frames(&vertex, &short_pixel, &long_pixel, xsdk_gpu_address(vertices), buffer);
	}
	xsdk_gpu_free(buffer);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&long_pixel);
	xsdk_shader_free(&short_pixel);
	xsdk_shader_free(&vertex);
	check("hiz: GPU close", NT_SUCCESS(xsdk_gpu_close()));
	return 0;
}
