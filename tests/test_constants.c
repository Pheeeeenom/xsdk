#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "probe_gpu.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// Six quads a frame from one vertex buffer and one pair of shaders, each placed and coloured only by its constants:
// set from the CPU, then loaded from memory.

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
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// Each pass of loop 0 adds this much blue.
	LOOP_STEP = 0x40,
	// The pixel shader's boolean 128 is bit 0 of word 4 (tools/gen_constant_shaders.py).
	BOOL_WORD = 4,
	// Points this far inside each corner hold the quad's colour, and this far outside its top left the clear colour.
	EDGE_MARGIN = 2,
	CHANNEL_TOLERANCE = 1,
	// The register indices of the constant split (xenia:src/xenia/gpu/register_table.inc:578-579).
	SQ_VS_CONST = 0x2307,
	SQ_PS_CONST = 0x2308,
	// Loaded constants come in groups of 4 from 64-byte aligned memory: each quad's two stages, one group each.
	GROUP = 4,
	GROUP_BYTES = GROUP * 16,
	SLOT_BYTES = 2 * GROUP_BYTES,
};

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	// The vertex shader's colour constant gives red.
	uint8_t red;
	// The pixel shader's c0 gives green, or its c1 when the boolean is set.
	uint8_t green;
	bool use_bool;
	uint8_t bool_green;
	// Loop 0's passes, each adding LOOP_STEP of blue.
	uint8_t loop_passes;
} Quad;

static const Quad quads[] = {
	{ "vertex constant only", 80, 80, 320, 240, 0xC0, 0x00, false, 0x00, 0 },
	{ "pixel constant", 480, 80, 320, 240, 0x00, 0xC0, false, 0x00, 0 },
	{ "boolean picks c1", 880, 80, 320, 240, 0x40, 0x20, true, 0xFF, 0 },
	{ "one loop pass", 80, 400, 320, 240, 0x40, 0x40, false, 0x00, 1 },
	{ "three loop passes", 480, 400, 320, 240, 0x00, 0x00, false, 0x00, 3 },
	{ "all of them", 880, 400, 320, 240, 0xFF, 0x20, true, 0x80, 2 },
};

// A unit square as two triangles; the vertex shader scales and moves it into place.
static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t as_stored(uint32_t argb)
{
	return __builtin_bswap32(argb);
}

static uint32_t expected_argb(const Quad *quad)
{
	const uint32_t green = quad->use_bool ? quad->bool_green : quad->green;
	return 0xFF000000 | (uint32_t)quad->red << 16 | green << 8 | (uint32_t)(quad->loop_passes * LOOP_STEP);
}

static bool channels_match(uint32_t stored, uint32_t want)
{
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static float unorm(uint8_t value)
{
	return value / 255.0f;
}

// Edges half a pixel early, as the other tests place theirs (xenia:src/xenia/gpu/draw_util.cc:837).
static void quad_floats(const Quad *quad, float vertex[GROUP][4], float pixel[GROUP][4])
{
	const float left = (quad->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (quad->x + quad->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (quad->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (quad->y + quad->height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex_values[GROUP][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, 0.0f, 0.0f },
		{ unorm(quad->red), 0.0f, 0.0f, 1.0f },
	};
	const float pixel_values[GROUP][4] = {
		{ 0.0f, unorm(quad->green), 0.0f, 0.0f },
		{ 0.0f, unorm(quad->bool_green), 0.0f, 0.0f },
		{ 0.0f, 0.0f, unorm(LOOP_STEP), 0.0f },
	};
	memcpy(vertex, vertex_values, sizeof vertex_values);
	memcpy(pixel, pixel_values, sizeof pixel_values);
}

// Each quad's float constants as memory to load from, one slot a quad.
static void fill_loads(volatile uint32_t *loads)
{
	for (uint32_t i = 0; i < sizeof quads / sizeof quads[0]; i++) {
		float floats[2][GROUP][4];
		quad_floats(&quads[i], floats[0], floats[1]);
		uint32_t words[SLOT_BYTES / 4];
		memcpy(words, floats, sizeof words);
		for (uint32_t w = 0; w < SLOT_BYTES / 4; w++)
			loads[i * SLOT_BYTES / 4 + w] = words[w];
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(loads), sizeof quads / sizeof quads[0] * SLOT_BYTES);
}

// From the CPU, or with loads_address, loaded from memory; the boolean and loop constants are always set.
static void set_quad_constants(uint32_t index, uint32_t loads_address)
{
	const Quad *const quad = &quads[index];
	if (loads_address) {
		xsdk_gpu_load_vertex_constants(0, loads_address + index * SLOT_BYTES, GROUP);
		xsdk_gpu_load_pixel_constants(0, loads_address + index * SLOT_BYTES + GROUP_BYTES, GROUP);
	} else {
		float vertex[GROUP][4];
		float pixel[GROUP][4];
		quad_floats(quad, vertex, pixel);
		xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
		xsdk_gpu_set_pixel_constants(0, &pixel[0][0], 3);
	}
	const uint32_t bools = quad->use_bool;
	const uint32_t loop = xsdk_gpu_loop_constant(quad->loop_passes, 0, 1);
	xsdk_gpu_set_bool_constants(BOOL_WORD, &bools, 1);
	xsdk_gpu_set_loop_constants(0, &loop, 1);
}

static bool point_holds(volatile const uint32_t *pixels, uint32_t frame, const char *name, uint32_t x, uint32_t y,
	uint32_t want_argb, bool exact, uint32_t *misses)
{
	const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
	const bool holds = exact ? stored == as_stored(want_argb) : channels_match(stored, as_stored(want_argb));
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("constants: frame %u: %s: (%u, %u) held %08X, not %08X", (unsigned)frame, name, (unsigned)x,
			(unsigned)y, (unsigned)stored, (unsigned)as_stored(want_argb));
	*misses += !holds;
	return holds;
}

// Each quad's centre and the points just inside its corners hold its colour; just outside its top left, the clear.
static bool frame_holds(volatile const uint32_t *pixels, uint32_t frame, uint32_t *misses)
{
	bool holds = true;
	for (uint32_t i = 0; i < sizeof quads / sizeof quads[0]; i++) {
		const Quad *const q = &quads[i];
		const uint32_t want = expected_argb(q);
		const uint32_t right = q->x + q->width - 1 - EDGE_MARGIN;
		const uint32_t bottom = q->y + q->height - 1 - EDGE_MARGIN;
		const uint32_t inside[5][2] = { { q->x + q->width / 2, q->y + q->height / 2 },
			{ q->x + EDGE_MARGIN, q->y + EDGE_MARGIN }, { right, q->y + EDGE_MARGIN }, { q->x + EDGE_MARGIN, bottom },
			{ right, bottom } };
		for (uint32_t p = 0; p < 5; p++)
			holds &= point_holds(pixels, frame, q->name, inside[p][0], inside[p][1], want, false, misses);
		holds &=
			point_holds(pixels, frame, q->name, q->x - 1 - EDGE_MARGIN, q->y + q->height / 2, CLEAR_ARGB, true, misses);
		holds &=
			point_holds(pixels, frame, q->name, q->x + q->width / 2, q->y - 1 - EDGE_MARGIN, CLEAR_ARGB, true, misses);
	}
	return holds;
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

// Each frame sets every quad's constants and draws it, resolves into the buffer not on screen, and shows it.
static void draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *const *buffers, bool has_display, uint32_t loads_address)
{
	const char *const how = loads_address ? ", loaded from memory" : "";
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		for (uint32_t i = 0; i < sizeof quads / sizeof quads[0]; i++) {
			set_quad_constants(i, loads_address);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		}
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("constants: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed)
			frames_wrong += !frame_holds(buffer, frame, &misses);
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("constants%s: %u of %u frames checked held a wrong point; %u points wrong", how,
		(unsigned)frames_wrong, (unsigned)(FRAMES - 1), (unsigned)misses);
	xsdk_log_write("constants: SQ_VS_CONST %08X, SQ_PS_CONST %08X after the frames",
		(unsigned)probe_gpu_registers[SQ_VS_CONST], (unsigned)probe_gpu_registers[SQ_PS_CONST]);
	char name[96];
	(void)snprintf(name, sizeof name, "constants: every frame's fence landed%s", how);
	check(name, fences_landed);
	(void)snprintf(name, sizeof name, "constants: every quad placed and coloured by its constants%s", how);
	check(name, fences_landed && frames_wrong == 0);
	if (has_display) {
		check("constants: every frame shown", shown);
		xsdk_log_write("constants: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

void test_constants(void)
{
	xsdk_log_write("constants: XAM had SQ_VS_CONST %08X, SQ_PS_CONST %08X", (unsigned)probe_gpu_registers[SQ_VS_CONST],
		(unsigned)probe_gpu_registers[SQ_PS_CONST]);
	const NTSTATUS status = xsdk_gpu_open();
	check("constants: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("constants: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, constant_pixel_shader, sizeof constant_pixel_shader));
	check("constants: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("constants: vertex fetch patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	volatile uint32_t *const loads = xsdk_gpu_alloc(sizeof quads / sizeof quads[0] * SLOT_BYTES);
	check("constants: vertex, front and constant buffers", vertices && buffers[0] && buffers[1] && loads);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1] && loads) {
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, has_display, 0);
		fill_loads(loads);
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, false, xsdk_gpu_address(loads));
	}
	xsdk_gpu_free(loads);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("constants: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
