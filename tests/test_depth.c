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

// Overlaps drawn both ways, a stencil mask and the cleared depth; with the depth test off, the depth checks must fail.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	DEPTH_OFF_FRAMES = 10,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 6,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	PANEL_Y = 220,
	OVERLAP_SIZE = 180,
	OVERLAP_STEP = 100,
	STENCIL_MARK = 1,
};

// The depth the resolves clear to: the near (0.25) and far (0.375) rectangles in front of it, one at 0.75 behind.
static const float clear_depth = 0.5f;

typedef enum {
	// Depth tested against what's drawn, as the done-when's overlaps are.
	PASS_DEPTH,
	// Stencil only.
	PASS_STENCIL_MARK,
	PASS_STENCIL_TEST,
} Pass;

typedef struct {
	const char *name;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	float depth;
	uint32_t argb;
	Pass pass;
} Rectangle;

// Drawn in this order each frame.
static const Rectangle rectangles[] = {
	// Back to front: far red, then near green over its lower right.
	{ "back to front, far", 40, PANEL_Y, OVERLAP_SIZE, OVERLAP_SIZE, 0.375f, 0xFFE04040, PASS_DEPTH },
	{ "back to front, near", 40 + OVERLAP_STEP, PANEL_Y + OVERLAP_STEP, OVERLAP_SIZE, OVERLAP_SIZE, 0.25f, 0xFF40E040,
		PASS_DEPTH },
	// Front to back: near blue, then far yellow, which the depth test keeps out of the overlap.
	{ "front to back, near", 360, PANEL_Y, OVERLAP_SIZE, OVERLAP_SIZE, 0.25f, 0xFF4040E0, PASS_DEPTH },
	{ "front to back, far", 360 + OVERLAP_STEP, PANEL_Y + OVERLAP_STEP, OVERLAP_SIZE, OVERLAP_SIZE, 0.375f, 0xFFE0E040,
		PASS_DEPTH },
	// A grey mask writes stencil, then magenta draws only where it did.
	{ "stencil mask", 750, PANEL_Y + 70, 140, 140, 0.25f, 0xFF808080, PASS_STENCIL_MARK },
	{ "stencil-tested", 680, PANEL_Y, 280, 280, 0.25f, 0xFFE040E0, PASS_STENCIL_TEST },
	// Against the cleared depth: orange behind it stays hidden, cyan in front shows.
	{ "behind the clear", 1000, PANEL_Y, 120, 280, 0.75f, 0xFFFF8000, PASS_DEPTH },
	{ "in front of the clear", 1160, PANEL_Y, 120, 280, 0.25f, 0xFF40E0E0, PASS_DEPTH },
};

typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	// Wrong with the depth test off: the checks that show depth testing.
	bool needs_depth;
} Point;

static const Point points[] = {
	{ "back to front: far alone", 40 + 40, PANEL_Y + 40, 0xFFE04040, false },
	{ "back to front: overlap", 40 + 140, PANEL_Y + 140, 0xFF40E040, false },
	{ "front to back: near alone", 360 + 40, PANEL_Y + 40, 0xFF4040E0, false },
	{ "front to back: overlap", 360 + 140, PANEL_Y + 140, 0xFF4040E0, true },
	{ "front to back: far alone", 360 + 240, PANEL_Y + 240, 0xFFE0E040, false },
	{ "stencil: inside the mask", 820, PANEL_Y + 140, 0xFFE040E0, false },
	{ "stencil: outside the mask", 700, PANEL_Y + 20, CLEAR_ARGB, false },
	{ "behind the clear: hidden", 1060, PANEL_Y + 140, CLEAR_ARGB, true },
	{ "in front of the clear: shown", 1220, PANEL_Y + 140, 0xFF40E0E0, false },
};

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

typedef struct {
	XsdkDepthStencilState depth;
	XsdkDepthStencilState depth_off;
	XsdkDepthStencilState stencil_mark;
	XsdkDepthStencilState stencil_test;
} States;

static void make_states(States *states)
{
	const XsdkDepthStencil depth = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	const XsdkDepthStencil depth_off = { .depth_test = false };
	const XsdkDepthStencil stencil_mark = { .stencil_test = true,
		.stencil_compare = XSDK_COMPARE_ALWAYS,
		.stencil_fail = XSDK_STENCIL_KEEP,
		.stencil_depth_fail = XSDK_STENCIL_KEEP,
		.stencil_pass = XSDK_STENCIL_REPLACE,
		.stencil_reference = STENCIL_MARK,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0xFF };
	const XsdkDepthStencil stencil_test = { .stencil_test = true,
		.stencil_compare = XSDK_COMPARE_EQUAL,
		.stencil_fail = XSDK_STENCIL_KEEP,
		.stencil_depth_fail = XSDK_STENCIL_KEEP,
		.stencil_pass = XSDK_STENCIL_KEEP,
		.stencil_reference = STENCIL_MARK,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0 };
	xsdk_gpu_depth_stencil_state(&states->depth, &depth);
	xsdk_gpu_depth_stencil_state(&states->depth_off, &depth_off);
	xsdk_gpu_depth_stencil_state(&states->stencil_mark, &stencil_mark);
	xsdk_gpu_depth_stencil_state(&states->stencil_test, &stencil_test);
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The constant vertex shader places the unit square, edges half a pixel early, at the depth in c1.z, coloured by c2.
static void set_rectangle_constants(const Rectangle *r)
{
	const float left = (r->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (r->x + r->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (r->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (r->y + r->height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, r->depth, 0.0f },
		{ unorm(r->argb >> 16), unorm(r->argb >> 8), unorm(r->argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
}

static bool matches(volatile const uint32_t *pixels, const Point *point)
{
	const uint32_t stored = pixels[tiled_index(point->x, point->y, WIDTH)];
	const uint32_t want = __builtin_bswap32(point->argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

typedef struct {
	uint32_t frames;
	uint32_t frames_wrong;
	// Frames where every depth check was wrong, and where every other check was right.
	uint32_t depth_all_wrong;
	uint32_t others_all_right;
	uint32_t misses_logged;
} Tally;

static void check_frame(volatile const uint32_t *pixels, uint32_t frame, bool depth_on, Tally *tally)
{
	bool all_right = true;
	bool depth_all_wrong = true;
	bool others_right = true;
	for (uint32_t i = 0; i < sizeof points / sizeof points[0]; i++) {
		const Point *const p = &points[i];
		const bool right = matches(pixels, p);
		all_right &= right;
		if (p->needs_depth)
			depth_all_wrong &= !right;
		else
			others_right &= right;
		if (!right && depth_on && tally->misses_logged < MAX_MISSES_LOGGED) {
			xsdk_log_write("depth: frame %u: %s at (%u, %u) held %08X, not %08X", (unsigned)frame, p->what,
				(unsigned)p->x, (unsigned)p->y, (unsigned)pixels[tiled_index(p->x, p->y, WIDTH)],
				(unsigned)__builtin_bswap32(p->argb));
			tally->misses_logged++;
		}
	}
	tally->frames++;
	tally->frames_wrong += !all_right;
	tally->depth_all_wrong += depth_all_wrong;
	tally->others_all_right += others_right;
}

static void draw_rectangles(const States *states, bool depth_on)
{
	for (uint32_t i = 0; i < sizeof rectangles / sizeof rectangles[0]; i++) {
		const Rectangle *const r = &rectangles[i];
		const XsdkDepthStencilState *state = depth_on ? &states->depth : &states->depth_off;
		if (r->pass == PASS_STENCIL_MARK)
			state = &states->stencil_mark;
		else if (r->pass == PASS_STENCIL_TEST)
			state = &states->stencil_test;
		xsdk_gpu_set_depth_stencil(state);
		set_rectangle_constants(r);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

// Each frame draws into eDRAM, resolves into the buffer not on screen, clearing colour, depth and stencil, and shows
// it.
static bool draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *const *buffers, bool has_display, bool depth_on, uint32_t frames, Tally *tally, bool *shown)
{
	States states;
	make_states(&states);
	for (uint32_t frame = 0; frame < frames; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		draw_rectangles(&states, depth_on);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS))) {
			xsdk_log_write(
				"depth: frame %u's fence didn't land, depth test %s", (unsigned)frame, depth_on ? "on" : "off");
			return false;
		}
		// The first frame's colour, depth and stencil are whatever eDRAM held; later ones are the clear values.
		if (frame > 0)
			check_frame(buffer, frame, depth_on, tally);
		if (has_display)
			*shown = *shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	return true;
}

static void run(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices, volatile uint32_t *const *buffers,
	bool has_display)
{
	Tally on = { 0 };
	Tally off = { 0 };
	bool shown = true;
	xsdk_gpu_set_depth_clear(clear_depth, 0);
	// Depth off first and unshown, so the frame left on screen for a capture is a depth-tested one.
	const bool landed_off = draw_frames(vertex, pixel, vertices, buffers, false, false, DEPTH_OFF_FRAMES, &off, &shown);
	const bool landed_on =
		landed_off && draw_frames(vertex, pixel, vertices, buffers, has_display, true, FRAMES, &on, &shown);
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_log_write("depth: depth test on: %u of %u frames checked held a wrong point", (unsigned)on.frames_wrong,
		(unsigned)on.frames);
	xsdk_log_write("depth: depth test off: %u of %u frames got every depth check wrong and every other right",
		(unsigned)(off.depth_all_wrong < off.others_all_right ? off.depth_all_wrong : off.others_all_right),
		(unsigned)off.frames);
	check("depth: every frame's fence landed", landed_on && landed_off);
	check("depth: overlaps, the stencil mask and the cleared depth all right with the depth test on",
		landed_on && on.frames == FRAMES - 1 && on.frames_wrong == 0);
	check("depth: with the depth test off, the depth checks fail and only they do",
		landed_off && off.frames == DEPTH_OFF_FRAMES - 1 && off.depth_all_wrong == off.frames &&
			off.others_all_right == off.frames);
	if (has_display) {
		check("depth: every frame shown", shown);
		xsdk_log_write("depth: the depth-tested frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
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

void test_depth(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("depth: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("depth: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("depth: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("depth: vertex fetch patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("depth: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1])
		run(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, has_display);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("depth: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
