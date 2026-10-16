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

// A pixel shader writing depth through oDepth (tools/gen_hiz_shaders.py), with SQ_PROGRAM_CNTL's ps_export_mode at
// libxenon's 2 and at 3; then whether the hierarchical buffer tests such a draw by its polygon's depth.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_US = 2000000,
	CLEAR_ARGB = 0xFF202020,
	FIRST_ARGB = 0xFF40E040,
	SECOND_ARGB = 0xFFE04040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	// SQ_PROGRAM_CNTL's ps_export_mode (xenia:src/xenia/gpu/registers.h:145).
	PS_EXPORT_MODE_SHIFT = 27,
	PS_EXPORT_MODE_MASK = 0xF << PS_EXPORT_MODE_SHIFT,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc).
	RB_HIZCONTROL = 0x2203,
};

// libxenon's colour mode (libxenon:drivers/xenos/xe.c:419), then with its low bit set.
static const uint32_t modes[] = { 2, 3 };

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
static XsdkShader plain_shader;
static XsdkShader depth_shader;
static uint32_t vertices_address;
static volatile uint32_t *buffer;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static bool fence_landed(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("depth export probe: fence %u didn't land", (unsigned)fence);
			return false;
		}
	}
	return true;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square over the target at the depth in c1.z, coloured by c2.
static void draw_full_screen(float depth, uint32_t argb)
{
	const float vertex[3][4] = {
		{ 2.0f, -2.0f, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH, 1.0f + 1.0f / HEIGHT, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static void set_depth(uint32_t compare)
{
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = compare };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
}

static void draw_writing(float polygon_depth, float written_depth, uint32_t argb)
{
	const float constant[4] = { written_depth, 0.0f, 0.0f, 0.0f };
	xsdk_gpu_set_pixel_constants(0, constant, 1);
	xsdk_gpu_set_shaders(&vertex_shader, &depth_shader);
	draw_full_screen(polygon_depth, argb);
}

static void draw_plain(float depth, uint32_t argb)
{
	xsdk_gpu_set_shaders(&vertex_shader, &plain_shader);
	draw_full_screen(depth, argb);
}

// A resolve clearing colour and depth to 1, then the target over it.
static bool begin(void)
{
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!fence_landed())
		return false;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	return true;
}

static uint32_t pixels_of(uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	uint32_t count = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = buffer[tiled_index(x, y, WIDTH)];
			bool right = true;
			for (uint32_t shift = 8; shift <= 24; shift += 8) {
				const uint32_t got = stored >> shift & 0xFF;
				const uint32_t wanted = want >> shift & 0xFF;
				right &= (got > wanted ? got - wanted : wanted - got) <= CHANNEL_TOLERANCE;
			}
			count += right;
		}
	}
	return count;
}

static bool end(const char *what, uint32_t mode)
{
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!fence_landed())
		return false;
	xsdk_log_write("depth export probe: mode %u: %s: of %u pixels, %u the first draw's colour, %u the second's",
		(unsigned)mode, what, (unsigned)PIXELS, (unsigned)pixels_of(FIRST_ARGB), (unsigned)pixels_of(SECOND_ARGB));
	return true;
}

// A draw at 0.25 writing 0.75, always, then one at 0.5, less-than, which passes only where 0.75 was written.
static bool probe_export(uint32_t mode)
{
	if (!begin())
		return false;
	set_depth(XSDK_COMPARE_ALWAYS);
	draw_writing(0.25f, 0.75f, FIRST_ARGB);
	set_depth(XSDK_COMPARE_LESS);
	draw_plain(0.5f, SECOND_ARGB);
	return end("0.25 writing 0.75, then 0.5 less-than", mode);
}

// A near draw at 0.25, then one at 0.75 writing 0.1, less-than, with the buffer as xsdk sets it and then off.
static bool probe_hiz(uint32_t mode, bool hiz_on)
{
	if (!begin())
		return false;
	set_depth(XSDK_COMPARE_LESS);
	draw_plain(0.25f, FIRST_ARGB);
	if (!hiz_on) {
		volatile uint32_t *const packet = xsdk_gpu_reserve(2);
		packet[0] = RB_HIZCONTROL;
		packet[1] = 0;
	}
	draw_writing(0.75f, 0.1f, SECOND_ARGB);
	return end(
		hiz_on ? "0.25, then 0.75 writing 0.1, the buffer on" : "0.25, then 0.75 writing 0.1, the buffer off", mode);
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&plain_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&depth_shader, depth_pixel_shader, sizeof depth_pixel_shader)) ||
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
	xsdk_log_write("depth export probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("depth export probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("depth export probe: set-up failed");
		return 1;
	}
	const uint32_t program_control = depth_shader.program_control;
	bool landed = true;
	for (uint32_t i = 0; landed && i < sizeof modes / sizeof modes[0]; i++) {
		xsdk_log_write("depth export probe: mode %u", (unsigned)modes[i]);
		depth_shader.program_control =
			(program_control & ~(uint32_t)PS_EXPORT_MODE_MASK) | modes[i] << PS_EXPORT_MODE_SHIFT;
		landed = probe_export(modes[i]) && probe_hiz(modes[i], true) && probe_hiz(modes[i], false);
	}
	xsdk_log_write("depth export probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
