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

// Hidden pixels timed with hierarchical depth on, as xsdk sets it, and off: full-screen quads through the long pixel
// shader behind a near one, less-than over a clear to 1 and greater-than over a clear to 0.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_US = 2000000,
	CLEAR_ARGB = 0xFF202020,
	NEAR_ARGB = 0xFF40E040,
	FAR_ARGB = 0xFFE04040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	HIDDEN_QUADS = 16,
	TIMED_RESOLVES = 16,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc).
	RB_HIZCONTROL = 0x2203,
};

typedef struct {
	const char *name;
	float clear_depth;
	uint32_t compare;
	float near_depth;
	float far_depth;
} Direction;

static const Direction directions[] = {
	{ "less-than over a clear to 1", 1.0f, XSDK_COMPARE_LESS, 0.25f, 0.75f },
	{ "greater-than over a clear to 0", 0.0f, XSDK_COMPARE_GREATER, 0.75f, 0.25f },
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

static XsdkShader vertex_shader;
static XsdkShader short_shader;
static XsdkShader long_shader;
static uint32_t vertices_address;
static volatile uint32_t *buffer;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

// Spins on the fence, so the time is the GPU's to within a few microseconds; 0 if it didn't land.
static uint64_t fence_time_us(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("hiz probe: fence %u didn't land", (unsigned)fence);
			return 0;
		}
	}
	return now_us();
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square over the target at the depth in c1.z, coloured by c2.
static void draw_full_screen(float depth, uint32_t argb, uint32_t count)
{
	const float vertex[3][4] = {
		{ 2.0f, -2.0f, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH, 1.0f + 1.0f / HEIGHT, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static uint32_t pixels_not(uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = buffer[tiled_index(x, y, WIDTH)];
			bool right = true;
			for (uint32_t shift = 8; shift <= 24; shift += 8) {
				const uint32_t got = stored >> shift & 0xFF;
				const uint32_t wanted = want >> shift & 0xFF;
				right &= (got > wanted ? got - wanted : wanted - got) <= CHANNEL_TOLERANCE;
			}
			wrong += !right;
		}
	}
	return wrong;
}

// A clear, the near quad, then the hidden ones timed; returns their time, or 0 if a fence didn't land.
static uint64_t time_hidden(const Direction *d, bool hiz_on, uint32_t *wrong)
{
	xsdk_gpu_set_depth_clear(d->clear_depth, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	const float zero[4] = { 0, 0, 0, 0 };
	xsdk_gpu_set_pixel_constants(0, zero, 1);
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = d->compare };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
	if (!hiz_on) {
		volatile uint32_t *const packet = xsdk_gpu_reserve(2);
		packet[0] = RB_HIZCONTROL;
		packet[1] = 0;
	}
	xsdk_gpu_set_shaders(&vertex_shader, &short_shader);
	draw_full_screen(d->near_depth, NEAR_ARGB, 1);
	xsdk_gpu_set_shaders(&vertex_shader, &long_shader);
	const uint64_t start = fence_time_us();
	draw_full_screen(d->far_depth, FAR_ARGB, HIDDEN_QUADS);
	const uint64_t end = fence_time_us();
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!start || !end || !fence_time_us())
		return 0;
	*wrong = pixels_not(NEAR_ARGB);
	return end - start;
}

// Resolves clearing depth also test the whole hierarchical buffer (src/gpu_resolve.c); this is what that costs.
static void time_resolves(void)
{
	for (uint32_t clears_depth = 0; clears_depth < 2; clears_depth++) {
		xsdk_gpu_set_resolve_clears(true, clears_depth);
		const uint64_t start = fence_time_us();
		for (uint32_t i = 0; i < TIMED_RESOLVES; i++) {
			xsdk_gpu_set_target(WIDTH, HEIGHT);
			xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
		}
		const uint64_t end = fence_time_us();
		if (!start || !end)
			break;
		xsdk_log_write("hiz probe: %u resolves clearing colour%s took %u us, %u us each", (unsigned)TIMED_RESOLVES,
			clears_depth ? " and depth" : "", (unsigned)(end - start), (unsigned)((end - start) / TIMED_RESOLVES));
	}
	xsdk_gpu_set_resolve_clears(true, true);
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&short_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&long_shader, long_pixel_shader, sizeof long_pixel_shader)) ||
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
	xsdk_log_write("hiz probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	for (uint32_t i = 0; i < sizeof directions / sizeof directions[0]; i++) {
		const Direction *const d = &directions[i];
		uint32_t wrong_on = 0;
		uint32_t wrong_off = 0;
		const uint64_t on_us = time_hidden(d, true, &wrong_on);
		const uint64_t off_us = time_hidden(d, false, &wrong_off);
		if (!on_us || !off_us)
			break;
		const uint64_t hidden = (uint64_t)PIXELS * HIDDEN_QUADS;
		xsdk_log_write(
			"hiz probe: %s: %u hidden quads took %u us on, %u Mpixel/s, and %u us off, %u Mpixel/s; "
			"on is %u.%u times as fast",
			d->name, (unsigned)HIDDEN_QUADS, (unsigned)on_us, (unsigned)(hidden / on_us), (unsigned)off_us,
			(unsigned)(hidden / off_us), (unsigned)(off_us / on_us), (unsigned)(off_us * 10 / on_us % 10));
		xsdk_log_write("hiz probe: %s: pixels not the near quad's colour: %u on, %u off", d->name, (unsigned)wrong_on,
			(unsigned)wrong_off);
	}
	xsdk_gpu_set_depth_clear(1.0f, 0);
	time_resolves();
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
