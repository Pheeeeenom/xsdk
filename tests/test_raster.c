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

// One cell per piece of raster and blend state, each checked by pixel against what the CPU works out.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CELL_WIDTH = 200,
	CELL_HEIGHT = 180,
	// Blends round once to 8 bits; the CPU's sums may land a step either side.
	BLEND_TOLERANCE = 2,
};

enum {
	BLEND_NONE,
	BLEND_ALPHA,
	BLEND_ADD,
	BLEND_SUBTRACT,
	BLEND_MAX,
	BLEND_MASK_RED_BLUE,
	BLENDS,
};

enum {
	RASTER_PLAIN,
	RASTER_CULL_BACK,
	RASTER_CULL_FRONT,
	RASTER_BIAS,
	RASTERS,
};

// Pulls the biased rectangle toward the viewer, well past one 24-bit step.
static const float depth_bias = -0.01f;

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	// Alpha in the top byte.
	uint32_t argb;
	uint32_t blend;
	uint32_t raster;
	// Placed right to left, which reverses the winding.
	bool mirrored;
	bool depth_test;
	bool scissor;
	// Drawn over all of clip space, into a viewport set to the rectangle.
	bool viewport;
} Draw;

// Cells 200x180: x 40, 290, 540, 790 and 1040; y 120 and 420.
static const Draw draws[] = {
	// Alpha blend: half-transparent orange over the right of a blue base.
	{ 40, 120, 200, 180, 0xFF2080E0, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 140, 120, 100, 180, 0x80E08020, BLEND_ALPHA, RASTER_PLAIN, false, false, false, false },
	// Additive.
	{ 290, 120, 200, 180, 0xFF6040C0, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 390, 120, 100, 180, 0xFF60E010, BLEND_ADD, RASTER_PLAIN, false, false, false, false },
	// Subtract, source less destination.
	{ 540, 120, 200, 180, 0xFF40C020, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 640, 120, 100, 180, 0xFFE04080, BLEND_SUBTRACT, RASTER_PLAIN, false, false, false, false },
	// Max.
	{ 790, 120, 200, 180, 0xFF30D050, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 890, 120, 100, 180, 0xFFC04090, BLEND_MAX, RASTER_PLAIN, false, false, false, false },
	// Write mask: only red and blue of the overlay land.
	{ 1040, 120, 200, 180, 0xFF20E020, BLEND_NONE, RASTER_PLAIN, false, false, false, false },
	{ 1140, 120, 100, 180, 0xFFE020E0, BLEND_MASK_RED_BLUE, RASTER_PLAIN, false, false, false, false },
	// Cull back with clockwise front faces: a clockwise rectangle, then a mirrored one.
	{ 40, 420, 90, 180, 0xFFE04040, BLEND_NONE, RASTER_CULL_BACK, false, false, false, false },
	{ 150, 420, 90, 180, 0xFF40E040, BLEND_NONE, RASTER_CULL_BACK, true, false, false, false },
	// Cull front.
	{ 290, 420, 90, 180, 0xFF4040E0, BLEND_NONE, RASTER_CULL_FRONT, false, false, false, false },
	{ 400, 420, 90, 180, 0xFFE0E040, BLEND_NONE, RASTER_CULL_FRONT, true, false, false, false },
	// Scissor: a rectangle over the whole cell, cut to x 580-699 and y 450-569.
	{ 540, 420, 200, 180, 0xFFE040E0, BLEND_NONE, RASTER_PLAIN, false, false, true, false },
	// Viewport: all of clip space into x 820-959, y 440-579.
	{ 820, 440, 140, 140, 0xFF40E0E0, BLEND_NONE, RASTER_PLAIN, false, false, false, true },
	// Depth bias: a base at 0.5, then the same depth on top, unbiased on the left and biased on the right.
	{ 1040, 420, 200, 180, 0xFF808080, BLEND_NONE, RASTER_PLAIN, false, true, false, false },
	{ 1040, 420, 100, 180, 0xFFFF8000, BLEND_NONE, RASTER_PLAIN, false, true, false, false },
	{ 1140, 420, 100, 180, 0xFFFF8000, BLEND_NONE, RASTER_BIAS, false, true, false, false },
};

static const uint32_t scissor_x = 580;
static const uint32_t scissor_y = 450;
static const uint32_t scissor_size = 120;

typedef struct {
	const char *what;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	uint32_t tolerance;
} Point;

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
	XsdkBlendState blends[BLENDS];
	XsdkRasterState rasters[RASTERS];
	XsdkDepthStencilState depth_off;
	XsdkDepthStencilState depth_less;
} States;

static void make_states(States *states)
{
	const XsdkBlend blends[BLENDS] = {
		[BLEND_NONE] = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_ALPHA] = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
			XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_ADD] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_SUBTRACT] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_SUBTRACT, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_MAX] = { XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_MAX, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL },
		[BLEND_MASK_RED_BLUE] = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_RED | XSDK_WRITE_BLUE },
	};
	const XsdkRaster rasters[RASTERS] = {
		[RASTER_PLAIN] = { XSDK_CULL_NONE, true, 0.0f, 0.0f },
		[RASTER_CULL_BACK] = { XSDK_CULL_BACK, true, 0.0f, 0.0f },
		[RASTER_CULL_FRONT] = { XSDK_CULL_FRONT, true, 0.0f, 0.0f },
		[RASTER_BIAS] = { XSDK_CULL_NONE, true, depth_bias, 0.0f },
	};
	for (uint32_t i = 0; i < BLENDS; i++)
		xsdk_gpu_blend_state(&states->blends[i], &blends[i]);
	for (uint32_t i = 0; i < RASTERS; i++)
		xsdk_gpu_raster_state(&states->rasters[i], &rasters[i]);
	const XsdkDepthStencil off = { .depth_test = false };
	const XsdkDepthStencil less = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&states->depth_off, &off);
	xsdk_gpu_depth_stencil_state(&states->depth_less, &less);
}

static float channel(uint32_t argb, uint32_t shift)
{
	return (argb >> shift & 0xFF) / 255.0f;
}

static uint32_t to_byte(float value)
{
	const float clamped = value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
	return (uint32_t)(clamped * 255.0f + 0.5f);
}

// What the colour channels of src drawn over dst come to, as the CPU works out each blend.
static uint32_t blended(uint32_t blend, uint32_t src, uint32_t dst)
{
	uint32_t argb = 0xFF000000;
	const float alpha = channel(src, 24);
	for (uint32_t shift = 0; shift <= 16; shift += 8) {
		const float s = channel(src, shift);
		const float d = channel(dst, shift);
		float out = s;
		if (blend == BLEND_ALPHA)
			out = s * alpha + d * (1.0f - alpha);
		else if (blend == BLEND_ADD)
			out = s + d;
		else if (blend == BLEND_SUBTRACT)
			out = s - d;
		else if (blend == BLEND_MAX)
			out = s > d ? s : d;
		else if (blend == BLEND_MASK_RED_BLUE && shift == 8)
			out = d;
		argb |= to_byte(out) << shift;
	}
	return argb;
}

// Screen-space signed area of the rectangle's first triangle, y down: positive is clockwise as it looks.
static bool clockwise(const Draw *draw)
{
	const float left = draw->mirrored ? (float)(draw->x + draw->width) : (float)draw->x;
	const float right = draw->mirrored ? (float)draw->x : (float)(draw->x + draw->width);
	const float top = (float)draw->y;
	const float bottom = (float)(draw->y + draw->height);
	// Its vertices 0, 1 and 2 are the top left, top right and bottom left, so the cross product reduces to this.
	return (right - left) * (bottom - top) > 0.0f;
}

static bool drawn_when_culled(const Draw *draw)
{
	const bool front = clockwise(draw);
	if (draw->raster == RASTER_CULL_BACK)
		return front;
	if (draw->raster == RASTER_CULL_FRONT)
		return !front;
	return true;
}

static uint32_t points_expected(Point *points)
{
	uint32_t n = 0;
	// The blend cells: the base alone, then the overlay over it.
	for (uint32_t i = 0; i < 10; i += 2) {
		const Draw *const base = &draws[i];
		const Draw *const top = &draws[i + 1];
		points[n++] = (Point){ "base alone", base->x + 50, base->y + 90, base->argb, 0 };
		points[n++] =
			(Point){ "blended", top->x + 50, top->y + 90, blended(top->blend, top->argb, base->argb), BLEND_TOLERANCE };
	}
	for (uint32_t i = 10; i < 14; i++) {
		const Draw *const d = &draws[i];
		points[n++] = (Point){ drawn_when_culled(d) ? "front face drawn" : "back face culled", d->x + 45, d->y + 90,
			drawn_when_culled(d) ? d->argb : CLEAR_ARGB, 0 };
	}
	const uint32_t scissor_colour = draws[14].argb;
	const uint32_t last = scissor_size - 1;
	points[n++] = (Point){ "scissor: top left inside", scissor_x, scissor_y, scissor_colour, 0 };
	points[n++] = (Point){ "scissor: bottom right inside", scissor_x + last, scissor_y + last, scissor_colour, 0 };
	points[n++] = (Point){ "scissor: left of it", scissor_x - 1, scissor_y, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "scissor: below it", scissor_x + last, scissor_y + scissor_size, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "scissor: right of it", scissor_x + scissor_size, scissor_y + last, CLEAR_ARGB, 0 };
	const Draw *const v = &draws[15];
	points[n++] = (Point){ "viewport: inside its top left", v->x + 2, v->y + 2, v->argb, 0 };
	points[n++] = (Point){ "viewport: inside its bottom right", v->x + v->width - 3, v->y + v->height - 3, v->argb, 0 };
	points[n++] = (Point){ "viewport: left of it", v->x - 3, v->y + 70, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "viewport: below it", v->x + 70, v->y + v->height + 2, CLEAR_ARGB, 0 };
	points[n++] = (Point){ "depth bias: unbiased, hidden", 1040 + 50, 420 + 90, draws[16].argb, 0 };
	points[n++] = (Point){ "depth bias: biased, shown", 1140 + 50, 420 + 90, draws[18].argb, 0 };
	return n;
}

// Places the unit square, edges half a pixel early, or over all of clip space for the viewport draw.
static void set_draw_constants(const Draw *d)
{
	float left = (d->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	float right = (d->x + d->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	float top = 1.0f - (d->y - 0.5f) / (HEIGHT / 2.0f);
	float bottom = 1.0f - (d->y + d->height - 0.5f) / (HEIGHT / 2.0f);
	if (d->viewport) {
		left = -1.0f;
		right = 1.0f;
		top = 1.0f;
		bottom = -1.0f;
	}
	if (d->mirrored) {
		const float swap = left;
		left = right;
		right = swap;
	}
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, 0.5f, 0.0f },
		{ channel(d->argb, 16), channel(d->argb, 8), channel(d->argb, 0), channel(d->argb, 24) },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
}

static void draw_all(const States *states)
{
	for (uint32_t i = 0; i < sizeof draws / sizeof draws[0]; i++) {
		const Draw *const d = &draws[i];
		xsdk_gpu_set_blend(&states->blends[d->blend]);
		xsdk_gpu_set_raster(&states->rasters[d->raster]);
		xsdk_gpu_set_depth_stencil(d->depth_test ? &states->depth_less : &states->depth_off);
		if (d->scissor)
			xsdk_gpu_set_scissor(scissor_x, scissor_y, scissor_size, scissor_size);
		if (d->viewport)
			xsdk_gpu_set_viewport(d->x, d->y, d->width, d->height, 0.0f, 1.0f);
		set_draw_constants(d);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		if (d->scissor)
			xsdk_gpu_set_scissor(0, 0, WIDTH, HEIGHT);
		if (d->viewport)
			xsdk_gpu_set_viewport(0.0f, 0.0f, WIDTH, HEIGHT, 0.0f, 1.0f);
	}
}

static bool point_holds(volatile const uint32_t *pixels, const Point *p, uint32_t frame, uint32_t *misses)
{
	const uint32_t stored = pixels[tiled_index(p->x, p->y, WIDTH)];
	const uint32_t want = __builtin_bswap32(p->argb);
	bool holds = true;
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		holds &= (got > wanted ? got - wanted : wanted - got) <= p->tolerance;
	}
	if (!holds && *misses < MAX_MISSES_LOGGED)
		xsdk_log_write("raster: frame %u: %s at (%u, %u) held %08X, not %08X", (unsigned)frame, p->what, (unsigned)p->x,
			(unsigned)p->y, (unsigned)stored, (unsigned)want);
	*misses += !holds;
	return holds;
}

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *const *buffers, bool has_display)
{
	States states;
	make_states(&states);
	// The depth bias cell needs depth cleared to 1.0, which a float rounding once turned into 0.
	xsdk_gpu_set_depth_clear(1.0f, 0);
	Point points[32];
	const uint32_t point_count = points_expected(points);
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
		draw_all(&states);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("raster: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's colour and depth are whatever eDRAM held; later ones are the clear values.
		if (frame > 0 && fences_landed) {
			bool holds = true;
			for (uint32_t i = 0; i < point_count; i++)
				holds &= point_holds(buffer, &points[i], frame, &misses);
			frames_wrong += !holds;
		}
		if (has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("raster: %u points a frame; %u of %u frames checked held a wrong one, %u points wrong",
		(unsigned)point_count, (unsigned)frames_wrong, (unsigned)(FRAMES - 1), (unsigned)misses);
	check("raster: every frame's fence landed", fences_landed);
	check("raster: blends, write mask, culling, scissor, viewport and depth bias as worked out",
		fences_landed && frames_wrong == 0);
	if (has_display) {
		check("raster: every frame shown", shown);
		xsdk_log_write("raster: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
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

void test_raster(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("raster: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("raster: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("raster: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	check("raster: vertex fetch patched for a float4 position", patched);
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("raster: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && vertices && buffers[0] && buffers[1])
		draw_frames(&vertex, &pixel, xsdk_gpu_address(vertices), buffers, has_display);
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("raster: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
