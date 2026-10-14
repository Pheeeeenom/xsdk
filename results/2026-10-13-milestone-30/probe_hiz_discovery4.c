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

// The fourth discovery run for RB_HIZCONTROL: in run 3 a draw's first triangle read an old value of the buffer in
// the block at its top right, after the buffer changed. Each candidate here goes after every change to the buffer and
// before every probe, through the sequences that showed it, full-screen and in a small rectangle. Built against xsdk
// with xsdk-probe.patch.

extern uint32_t xsdk_probe_resolve_hiz;

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_US = 3000000,
	CLEAR_ARGB = 0xFF202020,
	PROBE_ARGB = 0xFFE0E040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	MAP_WIDTH = 64,
	MAP_HEIGHT = 8,
	RB_HIZCONTROL = 0x2203,
	RB_HIZ_CLEAR = 0x231C,
	WAIT_UNTIL = 0x05C8,
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	PM4_EVENT_WRITE = 0x46,
	HIZ_WRITE = 1,
	HIZ_TEST = 2,
	HIZ_FAR = 0x7FF,
	SMALL_X = 400,
	SMALL_Y = 300,
	SMALL_SIZE = 200,
};

typedef enum {
	FIX_NONE,
	FIX_CACHE_FLUSH,
	FIX_SC_WAIT_WC,
	FIX_TILE_FLUSH,
	FIX_CACHE_FLUSH_AND_INV,
	FIX_CONTEXT_DONE,
	FIX_WAIT_IDLE,
	FIX_NEVER_DRAW,
	FIX_PIXEL_DRAW,
	FIX_VIZQUERY,
	FIXES,
} Fix;

static const char *const fix_names[FIXES] = { "nothing", "EVENT_WRITE CACHE_FLUSH", "EVENT_WRITE SC_WAIT_WC",
	"EVENT_WRITE TILE_FLUSH", "EVENT_WRITE CACHE_FLUSH_AND_INV_EVENT", "EVENT_WRITE CONTEXT_DONE",
	"WAIT_UNTIL 3D idle clean", "a never-passing full-screen draw testing the buffer",
	"a never-passing one-pixel draw testing the buffer", "EVENT_WRITE VIZQUERY_START, VIZQUERY_END" };

static Fix fix;
static bool failed;
static uint32_t total_off;
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

static void put_event(uint32_t event)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE, 1);
	packet[1] = event;
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

static void begin_target(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
}

static void rectangle(
	uint32_t x, uint32_t y, uint32_t w, uint32_t h, float depth, uint32_t compare, bool write, uint32_t argb, uint32_t hiz)
{
	xsdk_gpu_set_depth_stencil(&depth_states[compare][write]);
	put_register(RB_HIZCONTROL, hiz);
	const float left = (x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (x + w - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (y + h - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static void apply_fix(void)
{
	switch (fix) {
	case FIX_CACHE_FLUSH:
		put_event(6);
		break;
	case FIX_SC_WAIT_WC:
		put_event(9);
		break;
	case FIX_TILE_FLUSH:
		put_event(15);
		break;
	case FIX_CACHE_FLUSH_AND_INV:
		put_event(22);
		break;
	case FIX_CONTEXT_DONE:
		put_event(5);
		break;
	case FIX_WAIT_IDLE:
		put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
		break;
	case FIX_NEVER_DRAW:
		rectangle(0, 0, WIDTH, HEIGHT, 0.5f, XSDK_COMPARE_NEVER, false, CLEAR_ARGB, HIZ_TEST);
		break;
	case FIX_PIXEL_DRAW:
		rectangle(0, HEIGHT - 1, 1, 1, 0.5f, XSDK_COMPARE_NEVER, false, CLEAR_ARGB, HIZ_TEST);
		break;
	case FIX_VIZQUERY:
		put_event(7);
		put_event(8);
		break;
	default:
		break;
	}
}

// Resolves, clearing depth to 1 and the buffer to hiz_clear with RB_HIZCONTROL resolve_hiz.
static bool resolve(uint32_t resolve_hiz, uint32_t hiz_clear)
{
	xsdk_probe_resolve_hiz = resolve_hiz;
	put_register(RB_HIZ_CLEAR, hiz_clear);
	xsdk_gpu_resolve(buffer_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_probe_resolve_hiz = 0;
	const bool landed = wait_fence();
	begin_target();
	return landed;
}

static void clear(uint32_t hiz_clear)
{
	(void)resolve(HIZ_WRITE, hiz_clear);
	apply_fix();
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

// Logs where the probe drew inside its rectangle against everywhere or nowhere, and a map around the first pixel off.
static void report(const char *name, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, bool expect_everywhere)
{
	uint32_t off = 0;
	uint32_t left = WIDTH;
	uint32_t top = HEIGHT;
	uint32_t right = 0;
	uint32_t bottom = 0;
	for (uint32_t y = y0; y < y0 + h; y++) {
		for (uint32_t x = x0; x < x0 + w; x++) {
			if (has(x, y, PROBE_ARGB) == expect_everywhere)
				continue;
			off++;
			left = x < left ? x : left;
			top = y < top ? y : top;
			right = x > right ? x : right;
			bottom = y > bottom ? y : bottom;
		}
	}
	total_off += off;
	if (!off) {
		xsdk_log_write("hiz probe: %s, %s: right", fix_names[fix], name);
		return;
	}
	xsdk_log_write("hiz probe: %s, %s: %u pixels off in (%u, %u) to (%u, %u)", fix_names[fix], name, (unsigned)off,
		(unsigned)left, (unsigned)top, (unsigned)right, (unsigned)bottom);
	const uint32_t map_x = right + 1 >= MAP_WIDTH ? right + 1 - MAP_WIDTH : 0;
	for (uint32_t y = top; y < top + MAP_HEIGHT && y < HEIGHT; y++) {
		char row[MAP_WIDTH + 1];
		for (uint32_t x = 0; x < MAP_WIDTH; x++)
			row[x] = has(map_x + x, y, PROBE_ARGB) ? '#' : '.';
		row[MAP_WIDTH] = '\0';
		xsdk_log_write("hiz probe: %s, %s: probe drawn at (%u, %u): %s", fix_names[fix], name, (unsigned)map_x,
			(unsigned)y, row);
	}
}

static void probe(const char *name, uint32_t x, uint32_t y, uint32_t w, uint32_t h, float depth, bool expect_everywhere)
{
	if (failed)
		return;
	apply_fix();
	rectangle(x, y, w, h, depth, XSDK_COMPARE_LESS, false, PROBE_ARGB, HIZ_TEST);
	if (resolve(0, 0))
		report(name, x, y, w, h, expect_everywhere);
}

static void sequences(void)
{
	clear(HIZ_FAR);
	clear(0x200);
	probe("cleared to 0.25 from far, probe at 0.4", 0, 0, WIDTH, HEIGHT, 0.4f, false);
	clear(HIZ_FAR);
	probe("cleared far from 0.25, probe at 0.9", 0, 0, WIDTH, HEIGHT, 0.9f, true);
	rectangle(0, 0, WIDTH, HEIGHT, 0.2f, XSDK_COMPARE_LESS, true, CLEAR_ARGB, HIZ_WRITE);
	apply_fix();
	rectangle(0, 0, WIDTH, HEIGHT, 1.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	probe("drawn at 0.2 over far, probe at 0.4", 0, 0, WIDTH, HEIGHT, 0.4f, false);
	clear(0x199);
	rectangle(0, 0, WIDTH, HEIGHT, 0.7f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, HIZ_WRITE);
	apply_fix();
	rectangle(0, 0, WIDTH, HEIGHT, 1.0f, XSDK_COMPARE_ALWAYS, true, CLEAR_ARGB, 0);
	probe("drawn at 0.7 with always over 0.2, probe at 0.6", 0, 0, WIDTH, HEIGHT, 0.6f, true);
	clear(HIZ_FAR);
	clear(0x200);
	probe("cleared to 0.25 from far, small probe at 0.4", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.4f, false);
	clear(HIZ_FAR);
	probe("cleared far from 0.25, small probe at 0.9", SMALL_X, SMALL_Y, SMALL_SIZE, SMALL_SIZE, 0.9f, true);
	clear(HIZ_FAR);
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
	buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
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

int main(void)
{
	xsdk_log_write("hiz probe 4");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	xsdk_gpu_set_depth_clear(1.0f, 0);
	begin_target();
	for (fix = FIX_NONE; fix < FIXES && !failed; fix++) {
		total_off = 0;
		sequences();
		xsdk_log_write("hiz probe: %s: %u pixels off in all", fix_names[fix], (unsigned)total_off);
	}
	xsdk_log_write("hiz probe: %s", failed ? "stopped early" : "done");
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
