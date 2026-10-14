#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// The fifth discovery run, against xsdk with hierarchical depth on: whether the buffer's test sees depth bias, and the
// draw after a depth clear over a near frame, with and without the resolve's SC_WAIT_WC (the probe's own clear).

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_US = 3000000,
	CLEAR_ARGB = 0xFF202020,
	BASE_ARGB = 0xFF40E040,
	DECAL_ARGB = 0xFFE0E040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	RB_HIZCONTROL = 0x2203,
	REPEATS = 4,
};

static XsdkShader vertex_shader;
static XsdkShader pixel_shader;
static uint32_t vertices_address;
static volatile uint32_t *buffer;
static bool failed;

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

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static bool wait_fence(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("hiz probe: fence %u didn't land", (unsigned)fence);
			failed = true;
			return false;
		}
	}
	return true;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

static void full(float depth, uint32_t compare, uint32_t argb, float bias, bool hiz_off)
{
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = compare };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
	if (hiz_off) {
		volatile uint32_t *const packet = xsdk_gpu_reserve(2);
		packet[0] = RB_HIZCONTROL;
		packet[1] = 0;
	}
	const XsdkRaster raster = { XSDK_CULL_NONE, true, bias, 0.0f };
	XsdkRasterState raster_state;
	xsdk_gpu_raster_state(&raster_state, &raster);
	xsdk_gpu_set_raster(&raster_state);
	const float vertex[3][4] = {
		{ 2.0f, -2.0f, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH, 1.0f + 1.0f / HEIGHT, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static void begin_target(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
}

static bool resolve(float clear_depth)
{
	xsdk_log_write("hiz probe: next, a resolve clearing depth to %.2f", (double)clear_depth);
	xsdk_gpu_set_depth_clear(clear_depth, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	const bool landed = wait_fence();
	begin_target();
	return landed;
}

static bool has(uint32_t x, uint32_t y, uint32_t argb)
{
	const uint32_t stored = buffer[tiled_index(x, y, WIDTH)];
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static void report(const char *name, uint32_t argb)
{
	uint32_t off = 0;
	uint32_t left = WIDTH;
	uint32_t top = HEIGHT;
	uint32_t right = 0;
	uint32_t bottom = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			if (has(x, y, argb))
				continue;
			off++;
			left = x < left ? x : left;
			top = y < top ? y : top;
			right = x > right ? x : right;
			bottom = y > bottom ? y : bottom;
		}
	}
	if (off)
		xsdk_log_write("hiz probe: %s: %u pixels not %08X in (%u, %u) to (%u, %u)", name, (unsigned)off,
			(unsigned)argb, (unsigned)left, (unsigned)top, (unsigned)right, (unsigned)bottom);
	else
		xsdk_log_write("hiz probe: %s: every pixel %08X", name, (unsigned)argb);
}

// A base, then a decal behind it that a bias toward the viewer brings in front.
static void bias(const char *name, float clear_depth, uint32_t compare, float base, float decal, float decal_bias,
	bool hiz_off)
{
	if (failed || !resolve(clear_depth))
		return;
	full(base, compare, BASE_ARGB, 0.0f, false);
	full(decal, compare, DECAL_ARGB, decal_bias, hiz_off);
	if (resolve(clear_depth))
		report(name, DECAL_ARGB);
}

// Near over everything, a clear, then far over everything, which must all draw.
static void after_clear(const char *name, float clear_depth, uint32_t compare, float near, float far)
{
	for (uint32_t i = 0; i < REPEATS && !failed; i++) {
		if (!resolve(clear_depth))
			return;
		full(near, compare, BASE_ARGB, 0.0f, false);
		if (!resolve(clear_depth))
			return;
		full(far, compare, DECAL_ARGB, 0.0f, false);
		if (resolve(clear_depth))
			report(name, DECAL_ARGB);
	}
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertices || !buffer)
		return false;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(vertices_address, sizeof square);
	return true;
}

int main(void)
{
	xsdk_log_write("hiz probe 5");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	begin_target();
	bias("less, decal at 0.51 biased by -0.02 over 0.5", 1.0f, XSDK_COMPARE_LESS, 0.5f, 0.51f, -0.02f, false);
	bias("less, the same with the buffer off", 1.0f, XSDK_COMPARE_LESS, 0.5f, 0.51f, -0.02f, true);
	bias("greater, decal at 0.49 biased by 0.02 over 0.5", 0.0f, XSDK_COMPARE_GREATER, 0.5f, 0.49f, 0.02f, false);
	bias("greater, the same with the buffer off", 0.0f, XSDK_COMPARE_GREATER, 0.5f, 0.49f, 0.02f, true);
	after_clear("less, far at 0.7 after a clear over near at 0.1", 1.0f, XSDK_COMPARE_LESS, 0.1f, 0.7f);
	after_clear("greater, far at 0.3 after a clear over near at 0.9", 0.0f, XSDK_COMPARE_GREATER, 0.9f, 0.3f);
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_log_write("hiz probe: %s", failed ? "stopped early" : "done");
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
