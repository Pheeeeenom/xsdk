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

// The third discovery run for RB_HIZCONTROL: where the buffer keeps an old value after a clear or a draw (the top
// right corner of runs 1 and 2), RB_HIZ_CLEAR's scale at 0.5 and near 1, the greater-than direction, and the largest
// target it covers. Each phase sets what the buffer should hold, with depth at 1 (or 0) everywhere, then draws a probe
// quad that only the buffer can reject, and logs where the probe drew against where it should have. Built against
// xsdk with xsdk-probe.patch: the probe picks the resolve's RB_HIZCONTROL and the resolve rectangle's top right y.

extern uint32_t xsdk_probe_resolve_hiz;
extern float xsdk_probe_rectangle_top_right_y;

enum {
	MAX_WIDTH = 1280,
	MAX_HEIGHT = 1024,
	FENCE_TIMEOUT_US = 3000000,
	CLEAR_ARGB = 0xFF202020,
	PROBE_ARGB = 0xFFE0E040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	MAP_WIDTH = 64,
	MAP_HEIGHT = 16,
	COLUMNS = 16,
	RB_HIZCONTROL = 0x2203,
	RB_HIZ_CLEAR = 0x231C,
	HIZ_WRITE = 1,
	HIZ_TEST = 2,
	HIZ_GREATER = 0x10,
	HIZ_FAR = 0x7FF,
};

static uint32_t width = 1280;
static uint32_t height = 720;
static bool failed;
static XsdkShader vertex_shader;
static XsdkShader pixel_shader;
static XsdkDepthStencilState depth_states[8][2];
static uint32_t vertices_address;
static volatile uint32_t *buffer;
static uint32_t buffer_address;

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

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

static void begin_target(void)
{
	xsdk_gpu_set_target(width, height);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
}

static void rectangle(
	uint32_t x, uint32_t y, uint32_t w, uint32_t h, float depth, uint32_t compare, bool write, uint32_t argb, uint32_t hiz)
{
	xsdk_gpu_set_depth_stencil(&depth_states[compare][write]);
	put_register(RB_HIZCONTROL, hiz);
	const float left = (x - 0.5f) / (width / 2.0f) - 1.0f;
	const float right = (x + w - 0.5f) / (width / 2.0f) - 1.0f;
	const float top = 1.0f - (y - 0.5f) / (height / 2.0f);
	const float bottom = 1.0f - (y + h - 0.5f) / (height / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static void full(float depth, uint32_t compare, bool write, uint32_t argb, uint32_t hiz)
{
	rectangle(0, 0, width, height, depth, compare, write, argb, hiz);
}

// Resolves, clearing depth to the last depth clear and the buffer to hiz_clear with RB_HIZCONTROL resolve_hiz.
static bool resolve(uint32_t resolve_hiz, uint32_t hiz_clear)
{
	xsdk_probe_resolve_hiz = resolve_hiz;
	put_register(RB_HIZ_CLEAR, hiz_clear);
	xsdk_gpu_resolve(buffer_address, width, height, CLEAR_ARGB);
	xsdk_probe_resolve_hiz = 0;
	const bool landed = wait_fence();
	begin_target();
	return landed;
}

static bool has(uint32_t x, uint32_t y, uint32_t argb)
{
	const uint32_t stored = buffer[tiled_index(x, y, width)];
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

// Logs where the probe drew against everywhere or nowhere, and the top right corner if any pixel was off.
static void report(const char *name, bool expect_everywhere)
{
	uint32_t off = 0;
	uint32_t left = width;
	uint32_t top = height;
	uint32_t right = 0;
	uint32_t bottom = 0;
	for (uint32_t y = 0; y < height; y++) {
		for (uint32_t x = 0; x < width; x++) {
			if (has(x, y, PROBE_ARGB) == expect_everywhere)
				continue;
			off++;
			left = x < left ? x : left;
			top = y < top ? y : top;
			right = x > right ? x : right;
			bottom = y > bottom ? y : bottom;
		}
	}
	if (!off) {
		xsdk_log_write("hiz probe: %s: the probe drew %s, as it should", name, expect_everywhere ? "everywhere" : "nowhere");
		return;
	}
	xsdk_log_write("hiz probe: %s: the probe should draw %s; %u pixels off in (%u, %u) to (%u, %u)", name,
		expect_everywhere ? "everywhere" : "nowhere", (unsigned)off, (unsigned)left, (unsigned)top, (unsigned)right,
		(unsigned)bottom);
	const uint32_t x0 = left / MAP_WIDTH * MAP_WIDTH;
	const uint32_t y0 = top / MAP_HEIGHT * MAP_HEIGHT;
	for (uint32_t y = y0; y < y0 + MAP_HEIGHT && y < height; y++) {
		char row[MAP_WIDTH + 1];
		for (uint32_t x = 0; x < MAP_WIDTH; x++)
			row[x] = x0 + x < width && has(x0 + x, y, PROBE_ARGB) ? '#' : '.';
		row[MAP_WIDTH] = '\0';
		xsdk_log_write("hiz probe: %s: probe drawn at (%u, %u): %s", name, (unsigned)x0, (unsigned)y, row);
	}
}

// Draws the probe, which depth always passes, then resolves and reports.
static void probe(const char *name, float depth, uint32_t compare, uint32_t hiz, bool expect_everywhere)
{
	if (failed)
		return;
	full(depth, compare, false, PROBE_ARGB, hiz);
	if (resolve(0, 0))
		report(name, expect_everywhere);
}

static void set_depth_clear(float depth)
{
	xsdk_gpu_set_depth_clear(depth, 0);
}

// The buffer set by a resolve, then by draws, each probed above and below.
static void corner(const char *size)
{
	char name[96];
	(void)resolve(HIZ_WRITE, HIZ_FAR);
	(void)resolve(HIZ_WRITE, HIZ_FAR);
	snprintf(name, sizeof name, "%s, cleared far, probe at 0.9", size);
	probe(name, 0.9f, XSDK_COMPARE_LESS, HIZ_TEST, true);
	(void)resolve(HIZ_WRITE, 0x200);
	snprintf(name, sizeof name, "%s, cleared to 0.25 from far, probe at 0.4", size);
	probe(name, 0.4f, XSDK_COMPARE_LESS, HIZ_TEST, false);
	(void)resolve(HIZ_WRITE, 0x200);
	snprintf(name, sizeof name, "%s, cleared to 0.25 again, probe at 0.1", size);
	probe(name, 0.1f, XSDK_COMPARE_LESS, HIZ_TEST, true);
	(void)resolve(HIZ_WRITE, HIZ_FAR);
	snprintf(name, sizeof name, "%s, cleared far from 0.25, probe at 0.9", size);
	probe(name, 0.9f, XSDK_COMPARE_LESS, HIZ_TEST, true);
	// Lowered by a draw: 0.2 written with the buffer, then depth put back to 1 without it.
	full(0.2f, XSDK_COMPARE_LESS, true, CLEAR_ARGB, HIZ_WRITE);
	full(1.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	snprintf(name, sizeof name, "%s, drawn at 0.2 over far, probe at 0.4", size);
	probe(name, 0.4f, XSDK_COMPARE_LESS, HIZ_TEST, false);
	// Raised by a draw: 0.7 written with always over 0.2.
	(void)resolve(HIZ_WRITE, 0x199);
	full(0.7f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, HIZ_WRITE);
	full(1.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	snprintf(name, sizeof name, "%s, drawn at 0.7 with always over 0.2, probe at 0.6", size);
	probe(name, 0.6f, XSDK_COMPARE_LESS, HIZ_TEST, true);
	(void)resolve(HIZ_WRITE, HIZ_FAR);
}

// Columns at 0.5 + (i - 4) / 16384 against a buffer cleared to 0x400, then at 1 - 2^-(i + 5) against 0x7FF.
static void scale(void)
{
	for (uint32_t pass = 0; pass < 2 && !failed; pass++) {
		(void)resolve(HIZ_WRITE, pass ? HIZ_FAR : 0x400);
		uint32_t drawn[COLUMNS];
		float depths[COLUMNS];
		for (uint32_t i = 0; i < COLUMNS; i++) {
			depths[i] = pass ? 1.0f - 1.0f / (float)(1u << (i + 5)) : 0.5f + ((float)i - 4.0f) / 16384.0f;
			if (pass && i == COLUMNS - 1)
				depths[i] = 1.0f;
			rectangle(i * (width / COLUMNS), 0, width / COLUMNS, height, depths[i],
				pass && i == COLUMNS - 1 ? XSDK_COMPARE_LESS_EQUAL : XSDK_COMPARE_LESS, false, PROBE_ARGB, HIZ_TEST);
		}
		if (!resolve(0, 0))
			return;
		for (uint32_t i = 0; i < COLUMNS; i++) {
			drawn[i] = 0;
			for (uint32_t y = 0; y < height; y++)
				for (uint32_t x = i * (width / COLUMNS); x < (i + 1) * (width / COLUMNS); x++)
					drawn[i] += has(x, y, PROBE_ARGB);
		}
		for (uint32_t i = 0; i < COLUMNS; i++)
			xsdk_log_write("hiz probe: scale, buffer %s: column at %.8f drew %u of %u pixels", pass ? "7FF" : "400",
				(double)depths[i], (unsigned)drawn[i], (unsigned)(width / COLUMNS * height));
	}
	(void)resolve(HIZ_WRITE, HIZ_FAR);
}

// Greater-than with RB_HIZCONTROL bit 4: cleared to 0, a draw at 0.6, then probes at 0.5 and 0.7.
static void greater(void)
{
	set_depth_clear(0.0f);
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0);
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0);
	probe("greater, cleared to 0, probe at 0.1", 0.1f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, true);
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0x400);
	probe("greater, cleared to 0.5, probe at 0.4", 0.4f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, false);
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0x400);
	probe("greater, cleared to 0.5, probe at 0.6", 0.6f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, true);
	// Raised by a draw at 0.6 over 0, then depth put back to 0 without the buffer.
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0);
	full(0.6f, XSDK_COMPARE_GREATER, true, CLEAR_ARGB, HIZ_WRITE | HIZ_GREATER);
	full(0.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	probe("greater, drawn at 0.6 over 0, probe at 0.5", 0.5f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, false);
	(void)resolve(HIZ_WRITE | HIZ_GREATER, 0);
	full(0.6f, XSDK_COMPARE_GREATER, true, CLEAR_ARGB, HIZ_WRITE | HIZ_GREATER);
	full(0.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	probe("greater, drawn at 0.6 over 0, probe at 0.7", 0.7f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, true);
	// The direction from the draw alone: a clear without bit 4.
	(void)resolve(HIZ_WRITE, 0);
	probe("greater, cleared to 0 without bit 4, probe at 0.1", 0.1f, XSDK_COMPARE_GREATER, HIZ_TEST | HIZ_GREATER, true);
	set_depth_clear(1.0f);
	(void)resolve(HIZ_WRITE, HIZ_FAR);
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	for (uint32_t compare = 0; compare < 8; compare++) {
		for (uint32_t write = 0; write < 2; write++) {
			const XsdkDepthStencil setting = { .depth_test = true, .depth_write = write, .depth_compare = compare };
			xsdk_gpu_depth_stencil_state(&depth_states[compare][write], &setting);
		}
	}
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(MAX_WIDTH, MAX_HEIGHT));
	if (!vertices || !buffer)
		return false;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(vertices);
	buffer_address = xsdk_gpu_address(buffer);
	xsdk_gpu_invalidate(vertices_address, sizeof square);
	return true;
}

static void size(uint32_t w, uint32_t h, const char *name)
{
	width = w;
	height = h;
	begin_target();
	(void)resolve(HIZ_WRITE, HIZ_FAR);
	corner(name);
}

int main(void)
{
	xsdk_log_write("hiz probe 3");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	set_depth_clear(1.0f);
	size(1280, 720, "1280x720");
	scale();
	greater();
	size(640, 480, "640x480");
	size(1280, 1024, "1280x1024");
	// The resolve rectangle's top right at -0.5, as its other corners, for a size not resolved yet.
	xsdk_probe_rectangle_top_right_y = -0.5f;
	size(1280, 704, "1280x704, square rectangle");
	xsdk_probe_rectangle_top_right_y = 0.0f;
	size(1280, 712, "1280x712");
	xsdk_log_write("hiz probe: %s", failed ? "stopped early" : "done");
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
