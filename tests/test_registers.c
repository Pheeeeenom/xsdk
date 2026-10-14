#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "register_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// Shaders past 16 registers (SCOPE.md, milestone 24): pixel shaders using 18 and 64 and a vertex shader using 64,
// each filling every register it has with a constant of its own and adding up how far each is off into its colour
// (tools/gen_register_shaders.py). Under libxenon's split, which gives pixel shaders 16, the first draw hangs the GPU.

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
	// Points this far inside each corner hold the quad's colour, and this far outside its top left the clear colour.
	EDGE_MARGIN = 2,
	CHANNEL_TOLERANCE = 1,
	// The most constants a shader here reads: the vertex shader's c0 to c3, then r2 to r63's fills from c4.
	MAX_CONSTANTS = 66,
	// tools/gen_register_shaders.py's constants. Both vertex shaders place the quad with c0 and c1 and colour it with
	// c2; the 64-register one takes its gain from c3 and fills r2 up from c4. The pixel shaders take their gain from c0
	// and fill r1 up from c1.
	VERTEX_SCALE = 0,
	VERTEX_OFFSET = 1,
	VERTEX_COLOR = 2,
	VERTEX_GAIN = 3,
	VERTEX_FILL_OFFSET = 2,
	PIXEL_GAIN = 0,
	PIXEL_FILL_OFFSET = 0,
	// A register's fill has x of its number over this, so any two differ by 1/64 at least.
	FILL_STEPS = 64,
};

// Squared, a 1/64 difference times this is past 1, so one register off washes the colour out to white.
static const float GAIN = 65536.0f;

typedef struct {
	const char *name;
	const uint32_t *vertex_shader;
	uint32_t vertex_shader_bytes;
	const uint32_t *pixel_shader;
	uint32_t pixel_shader_bytes;
	// The registers each stage fills: 0 for the ordinary shader on the other side.
	uint32_t vertex_registers;
	uint32_t pixel_registers;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	uint32_t argb;
} Case;

static const Case cases[] = {
	{ "a pixel shader using 18 registers", constant_vertex_shader, sizeof constant_vertex_shader,
		register_pixel_shader_18, sizeof register_pixel_shader_18, 0, 18, 80, 200, 320, 320, 0xFF208020 },
	{ "a pixel shader using 64 registers", constant_vertex_shader, sizeof constant_vertex_shader,
		register_pixel_shader_64, sizeof register_pixel_shader_64, 0, 64, 480, 200, 320, 320, 0xFF202080 },
	{ "a vertex shader using 64 registers", register_vertex_shader_64, sizeof register_vertex_shader_64,
		triangle_pixel_shader, sizeof triangle_pixel_shader, 64, 0, 880, 200, 320, 320, 0xFF808020 },
};

enum { CASES = sizeof cases / sizeof cases[0] };

// Each case's shaders, loaded.
static XsdkShader vertex_shaders[CASES];
static XsdkShader pixel_shaders[CASES];

// A unit square as two triangles; the vertex shaders scale and move it into place.
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

static float channel(uint32_t argb, uint32_t shift)
{
	return (argb >> shift & 0xFF) / 255.0f;
}

// Register r's fill, which no other register's matches.
static void put_fill(float *constant, uint32_t r)
{
	const float step = (float)r / FILL_STEPS;
	const float fill[4] = { step, 1.0f - step, 0.5f * step, 0.25f };
	memcpy(constant, fill, sizeof fill);
}

static void put_vector(float *constant, float x, float y, float z, float w)
{
	const float vector[4] = { x, y, z, w };
	memcpy(constant, vector, sizeof vector);
}

// Edges half a pixel early, as the other tests place theirs (xenia:src/xenia/gpu/draw_util.cc:837).
static void set_constants(const Case *c)
{
	float vertex[MAX_CONSTANTS][4] = { 0 };
	float pixel[MAX_CONSTANTS][4] = { 0 };
	const float left = (c->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (c->x + c->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (c->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (c->y + c->height - 0.5f) / (HEIGHT / 2.0f);
	put_vector(vertex[VERTEX_SCALE], right - left, bottom - top, 1.0f, 1.0f);
	put_vector(vertex[VERTEX_OFFSET], left, top, 0.0f, 0.0f);
	put_vector(vertex[VERTEX_COLOR], channel(c->argb, 16), channel(c->argb, 8), channel(c->argb, 0), 1.0f);
	uint32_t vertex_count = VERTEX_COLOR + 1;
	if (c->vertex_registers) {
		put_vector(vertex[VERTEX_GAIN], GAIN, GAIN, GAIN, GAIN);
		for (uint32_t r = 2; r < c->vertex_registers; r++)
			put_fill(vertex[r + VERTEX_FILL_OFFSET], r);
		vertex_count = c->vertex_registers + VERTEX_FILL_OFFSET;
	}
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], vertex_count);
	if (c->pixel_registers) {
		put_vector(pixel[PIXEL_GAIN], GAIN, GAIN, GAIN, GAIN);
		for (uint32_t r = 1; r < c->pixel_registers; r++)
			put_fill(pixel[r + PIXEL_FILL_OFFSET], r);
		xsdk_gpu_set_pixel_constants(0, &pixel[0][0], c->pixel_registers + PIXEL_FILL_OFFSET);
	}
}

static bool point_holds(volatile const uint32_t *pixels, uint32_t frame, const Case *c, uint32_t x, uint32_t y,
	uint32_t want_argb, bool exact, uint32_t *misses)
{
	const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
	const bool holds = exact ? stored == as_stored(want_argb) : channels_match(stored, as_stored(want_argb));
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("registers: frame %u: %s: (%u, %u) held %08X, not %08X", (unsigned)frame, c->name, (unsigned)x,
			(unsigned)y, (unsigned)stored, (unsigned)as_stored(want_argb));
	*misses += !holds;
	return holds;
}

// The quad's centre and the points just inside its corners hold its colour; just outside its top left, the clear.
static bool case_holds(volatile const uint32_t *pixels, uint32_t frame, const Case *c, uint32_t *misses)
{
	const uint32_t right = c->x + c->width - 1 - EDGE_MARGIN;
	const uint32_t bottom = c->y + c->height - 1 - EDGE_MARGIN;
	const uint32_t inside[5][2] = { { c->x + c->width / 2, c->y + c->height / 2 },
		{ c->x + EDGE_MARGIN, c->y + EDGE_MARGIN }, { right, c->y + EDGE_MARGIN }, { c->x + EDGE_MARGIN, bottom },
		{ right, bottom } };
	bool holds = true;
	for (uint32_t p = 0; p < 5; p++)
		holds &= point_holds(pixels, frame, c, inside[p][0], inside[p][1], c->argb, false, misses);
	holds &= point_holds(pixels, frame, c, c->x - 1 - EDGE_MARGIN, c->y + c->height / 2, CLEAR_ARGB, true, misses);
	holds &= point_holds(pixels, frame, c, c->x + c->width / 2, c->y - 1 - EDGE_MARGIN, CLEAR_ARGB, true, misses);
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

// Each frame draws the cases one at a time, waiting on each so a hang names its shader, then resolves into the buffer
// not on screen, checks it and shows it.
static void draw_frames(uint32_t vertices, volatile uint32_t *const *buffers, bool has_display)
{
	uint32_t misses[CASES] = { 0 };
	uint32_t frames_wrong[CASES] = { 0 };
	uint32_t frames_checked = 0;
	bool finished[CASES];
	for (uint32_t i = 0; i < CASES; i++)
		finished[i] = true;
	bool landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		for (uint32_t i = 0; i < CASES && landed; i++) {
			xsdk_gpu_set_shaders(&vertex_shaders[i], &pixel_shaders[i]);
			set_constants(&cases[i]);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
			finished[i] = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
			landed = finished[i];
			if (!landed)
				xsdk_log_write("registers: frame %u: %s: its draw never finished", (unsigned)frame, cases[i].name);
		}
		if (!landed)
			break;
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!landed)
			xsdk_log_write("registers: frame %u's resolve didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && landed) {
			for (uint32_t i = 0; i < CASES; i++)
				frames_wrong[i] += !case_holds(buffer, frame, &cases[i], &misses[i]);
			frames_checked++;
		}
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	for (uint32_t i = 0; i < CASES; i++) {
		xsdk_log_write("registers: %s: %u of %u frames checked held a wrong point; %u points wrong", cases[i].name,
			(unsigned)frames_wrong[i], (unsigned)frames_checked, (unsigned)misses[i]);
		char name[128];
		(void)snprintf(name, sizeof name, "registers: %s draws, every register carried into its colour", cases[i].name);
		check(name, landed && finished[i] && frames_checked == FRAMES - 1 && frames_wrong[i] == 0);
	}
	if (has_display)
		check("registers: every frame shown", landed && shown);
	if (has_display && landed) {
		xsdk_log_write("registers: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

void test_registers(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("registers: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("registers: display open at 1280x720", has_display);
	bool loaded = true;
	bool patched = true;
	for (uint32_t i = 0; i < CASES; i++) {
		const Case *const c = &cases[i];
		vertex_shaders[i] = (XsdkShader){ .microcode = NULL };
		pixel_shaders[i] = (XsdkShader){ .microcode = NULL };
		const bool both = NT_SUCCESS(xsdk_shader_load(&vertex_shaders[i], c->vertex_shader, c->vertex_shader_bytes)) &&
			NT_SUCCESS(xsdk_shader_load(&pixel_shaders[i], c->pixel_shader, c->pixel_shader_bytes));
		loaded &= both;
		patched &= both &&
			NT_SUCCESS(xsdk_shader_set_vertex_layout(
				&vertex_shaders[i], layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	}
	check("registers: shaders load", loaded);
	check("registers: vertex fetches patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("registers: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1]) {
		xsdk_log_write("registers: drawing with 18 and 64 pixel shader registers and 64 vertex shader registers");
		draw_frames(xsdk_gpu_address(vertices), buffers, has_display);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	for (uint32_t i = 0; i < CASES; i++) {
		xsdk_shader_free(&pixel_shaders[i]);
		xsdk_shader_free(&vertex_shaders[i]);
	}
	check("registers: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
