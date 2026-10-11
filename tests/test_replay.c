#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/present.h"
#include "xsdk/shader.h"

// A frame's draws recorded once and replayed every frame, paced: a background once and a bar twice, the ring moving
// each bar between replays.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FRAMES = 120,
	BUFFERS = 2,
	SWAP_INTERVAL = 1,
	BEGIN_TIMEOUT_MS = 1000,
	FENCE_TIMEOUT_MS = 1000,
	MAX_WRONG_LOGGED = 4,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	SQUARES = 4,
	SQUARE_SIZE = 100,
	BAR_WIDTH = 32,
	STEP = 8,
	// Where a bar can start, 8 pixels apart, all of it on screen.
	POSITIONS = (WIDTH - BAR_WIDTH) / STEP + 1,
	// One bar moves right and the other left, in rows of their own.
	BAR_A_TOP = 240,
	BAR_B_TOP = 390,
	BAR_HEIGHT = 90,
	COMMAND_DWORDS = 1024,
	// Too small for any call, so its recording overflows.
	TINY_DWORDS = 4,
};

typedef struct {
	uint32_t x;
	uint32_t y;
	uint32_t argb;
} Square;

static const Square squares[SQUARES] = {
	{ 60, 60, 0xFFE04040 },
	{ 1120, 60, 0xFF40E040 },
	{ 60, 560, 0xFF4040E0 },
	{ 1120, 560, 0xFFE0E040 },
};

static const uint32_t bar_a_argb = 0xFFFF00FF;
static const uint32_t bar_b_argb = 0xFF00FFFF;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static uint32_t bar_a_x(uint32_t frame)
{
	return frame % POSITIONS * STEP;
}

static uint32_t bar_b_x(uint32_t frame)
{
	return (POSITIONS - 1 - frame % POSITIONS) * STEP;
}

static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// A rectangle's two triangles, edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
static void put_rectangle(
	volatile uint32_t *vertices, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint32_t argb)
{
	const float left = clip_x(x - 0.5f);
	const float right = clip_x(x + width - 0.5f);
	const float top = clip_y(y - 0.5f);
	const float bottom = clip_y(y + height - 0.5f);
	const float corners[QUAD_VERTICES][2] = { { left, top }, { right, top }, { left, bottom }, { left, bottom },
		{ right, top }, { right, bottom } };
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float vertex[VERTEX_FLOATS] = { corners[v][0], corners[v][1], 0.0f, 1.0f, (argb >> 16 & 0xFF) / 255.0f,
			(argb >> 8 & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f, 1.0f };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, vertex, sizeof vertex);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			vertices[v * VERTEX_FLOATS + i] = words[i];
	}
}

// A bar at every position, one rectangle each.
static volatile uint32_t *upload_bars(uint32_t top, uint32_t argb)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(POSITIONS * QUAD_BYTES);
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < POSITIONS; i++)
		put_rectangle(vertices + i * QUAD_BYTES / 4, i * STEP, top, BAR_WIDTH, BAR_HEIGHT, argb);
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), POSITIONS * QUAD_BYTES);
	return vertices;
}

static volatile uint32_t *upload_squares(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(SQUARES * QUAD_BYTES);
	if (!vertices)
		return NULL;
	for (uint32_t i = 0; i < SQUARES; i++)
		put_rectangle(
			vertices + i * QUAD_BYTES / 4, squares[i].x, squares[i].y, SQUARE_SIZE, SQUARE_SIZE, squares[i].argb);
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), SQUARES * QUAD_BYTES);
	return vertices;
}

// A resolve stores the colour's bytes B, G, R, A, as the display reads them (docs/HARDWARE.md).
static uint32_t pixel_argb(volatile const uint32_t *pixels, uint32_t x, uint32_t y)
{
	return __builtin_bswap32(pixels[tiled_index(x, y, WIDTH)]);
}

// Each bar where its frame puts it, the clear colour half the frame away, and every square.
static bool frame_holds(volatile const uint32_t *pixels, uint32_t frame, bool log)
{
	const uint32_t a_y = BAR_A_TOP + BAR_HEIGHT / 2;
	const uint32_t b_y = BAR_B_TOP + BAR_HEIGHT / 2;
	const uint32_t checks[][3] = {
		{ bar_a_x(frame) + BAR_WIDTH / 2, a_y, bar_a_argb },
		{ (bar_a_x(frame) + WIDTH / 2) % WIDTH, a_y, CLEAR_ARGB },
		{ bar_b_x(frame) + BAR_WIDTH / 2, b_y, bar_b_argb },
		{ (bar_b_x(frame) + WIDTH / 2) % WIDTH, b_y, CLEAR_ARGB },
		{ squares[0].x + SQUARE_SIZE / 2, squares[0].y + SQUARE_SIZE / 2, squares[0].argb },
		{ squares[1].x + SQUARE_SIZE / 2, squares[1].y + SQUARE_SIZE / 2, squares[1].argb },
		{ squares[2].x + SQUARE_SIZE / 2, squares[2].y + SQUARE_SIZE / 2, squares[2].argb },
		{ squares[3].x + SQUARE_SIZE / 2, squares[3].y + SQUARE_SIZE / 2, squares[3].argb },
	};
	bool holds = true;
	for (uint32_t i = 0; i < sizeof checks / sizeof checks[0]; i++) {
		const uint32_t held = pixel_argb(pixels, checks[i][0], checks[i][1]);
		if (held == checks[i][2])
			continue;
		if (log)
			xsdk_log_write("replay: frame %u: (%u, %u) held %08X, not %08X", (unsigned)frame, (unsigned)checks[i][0],
				(unsigned)checks[i][1], (unsigned)held, (unsigned)checks[i][2]);
		holds = false;
	}
	return holds;
}

typedef struct {
	XsdkCommandBuffer background;
	XsdkCommandBuffer bar;
	uint32_t bar_a;
	uint32_t bar_b;
} Recorded;

// Runs the background once and the bar twice, the ring pointing stream 0 at each bar's place first.
static void draw_frame(const Recorded *recorded, uint32_t frame, uint32_t address)
{
	xsdk_gpu_execute(&recorded->background);
	xsdk_gpu_set_vertices(0, recorded->bar_a + frame % POSITIONS * QUAD_BYTES, QUAD_BYTES);
	xsdk_gpu_execute(&recorded->bar);
	xsdk_gpu_set_vertices(0, recorded->bar_b + (POSITIONS - 1 - frame % POSITIONS) * QUAD_BYTES, QUAD_BYTES);
	xsdk_gpu_execute(&recorded->bar);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
}

// Without a display, as in Xenia, the frames draw into two buffers, each checked once its fence lands.
static void draw_unpaced(const Recorded *recorded)
{
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	uint32_t wrong = 0;
	uint32_t drawn = 0;
	for (; drawn < FRAMES && buffers[0] && buffers[1]; drawn++) {
		volatile uint32_t *const buffer = buffers[drawn % BUFFERS];
		draw_frame(recorded, drawn, xsdk_gpu_address(buffer));
		if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
			break;
		wrong += !frame_holds(buffer, drawn, wrong < MAX_WRONG_LOGGED);
	}
	xsdk_log_write("replay: without a display, %u frames drawn, %u wrong", (unsigned)drawn, (unsigned)wrong);
	check("replay: every frame drawn from the recordings, both bars in place", drawn == FRAMES && wrong == 0);
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
}

// Presents FRAMES frames, checking each buffer when it comes back, as test_pacing does.
static void present_frames(const Recorded *recorded)
{
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS status = xsdk_present_open(XSDK_DISPLAY_720P, BUFFERS, SWAP_INTERVAL, &width, &height);
	check("replay: present open at 1280x720", NT_SUCCESS(status) && width == WIDTH && height == HEIGHT);
	if (!NT_SUCCESS(status)) {
		draw_unpaced(recorded);
		return;
	}
	uint32_t presented = 0;
	uint32_t address = 0;
	uint32_t wrong = 0;
	for (; presented < FRAMES; presented++) {
		if (!NT_SUCCESS(xsdk_present_begin(&address, BEGIN_TIMEOUT_MS)))
			break;
		// The buffer last held the frame BUFFERS before; frame 0's background is whatever eDRAM held.
		if (presented > BUFFERS && !frame_holds(xsdk_present_pixels(address), presented - BUFFERS, false)) {
			(void)frame_holds(xsdk_present_pixels(address), presented - BUFFERS, wrong < MAX_WRONG_LOGGED);
			wrong++;
		}
		draw_frame(recorded, presented, address);
		xsdk_present_end();
	}
	const bool drawn = presented == FRAMES && NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)) &&
		frame_holds(xsdk_present_pixels(address), FRAMES - 1, true);
	xsdk_log_write("replay: %u reused buffers held the wrong frame", (unsigned)wrong);
	xsdk_present_close();
	XsdkPresentStats stats;
	xsdk_present_stats(&stats);
	xsdk_log_write(
		"replay: %u frames, %u flips over %u vblanks (first %u, last %u), %u missed, %u late; longest wait %u us",
		(unsigned)presented, (unsigned)stats.flips, (unsigned)stats.vblanks, (unsigned)stats.first_flip_vblank,
		(unsigned)stats.last_flip_vblank, (unsigned)stats.missed, (unsigned)stats.late,
		(unsigned)stats.longest_wait_us);
	check("replay: every frame shown", presented == FRAMES && stats.flips == FRAMES);
	check("replay: no vblank missed", stats.flips == FRAMES && stats.missed == 0);
	check("replay: a flip every swap interval",
		stats.flips == FRAMES && stats.last_flip_vblank - stats.first_flip_vblank == (FRAMES - 1) * SWAP_INTERVAL);
	check("replay: every flip inside vblank", stats.flips == FRAMES && stats.late == 0);
	check("replay: every frame drawn from the recordings, both bars in place",
		presented == FRAMES && wrong == 0 && drawn);
}

// The background sets everything and draws the squares; the bar is just a draw, from whatever stream 0 holds.
static bool record(Recorded *recorded, const XsdkShader *vertex, const XsdkShader *pixel, uint32_t squares_address)
{
	xsdk_gpu_record_begin(&recorded->background);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(vertex, pixel);
	xsdk_gpu_set_vertices(0, squares_address, SQUARES * QUAD_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, SQUARES * QUAD_VERTICES);
	const bool background = NT_SUCCESS(xsdk_gpu_record_end());
	xsdk_gpu_record_begin(&recorded->bar);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	const bool bar = NT_SUCCESS(xsdk_gpu_record_end());
	xsdk_log_write("replay: the background took %u dwords, the bar %u", (unsigned)recorded->background.count,
		(unsigned)recorded->bar.count);
	return background && bar && recorded->background.count && recorded->bar.count;
}

// A recording too big for its buffer fails and leaves nothing to run.
static bool overflow_fails(void)
{
	XsdkCommandBuffer tiny;
	if (!NT_SUCCESS(xsdk_gpu_command_buffer_create(&tiny, TINY_DWORDS)))
		return false;
	xsdk_gpu_record_begin(&tiny);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	const NTSTATUS status = xsdk_gpu_record_end();
	const bool failed = status == STATUS_BUFFER_TOO_SMALL && tiny.count == 0;
	xsdk_gpu_command_buffer_free(&tiny);
	return failed;
}

void test_replay(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("replay: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	check("replay: a recording that outgrows its buffer fails", overflow_fails());
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	volatile uint32_t *const squares_vertices = upload_squares();
	volatile uint32_t *const bar_a = upload_bars(BAR_A_TOP, bar_a_argb);
	volatile uint32_t *const bar_b = upload_bars(BAR_B_TOP, bar_b_argb);
	__asm__ volatile("sync" ::: "memory");
	Recorded recorded = { .background = { .dwords = NULL }, .bar = { .dwords = NULL } };
	const bool made = loaded && squares_vertices && bar_a && bar_b &&
		NT_SUCCESS(xsdk_gpu_command_buffer_create(&recorded.background, COMMAND_DWORDS)) &&
		NT_SUCCESS(xsdk_gpu_command_buffer_create(&recorded.bar, COMMAND_DWORDS)) &&
		record(&recorded, &vertex, &pixel, xsdk_gpu_address(squares_vertices));
	check("replay: background and bar recorded", made);
	if (made) {
		recorded.bar_a = xsdk_gpu_address(bar_a);
		recorded.bar_b = xsdk_gpu_address(bar_b);
		present_frames(&recorded);
	}
	if (recorded.bar.dwords)
		xsdk_gpu_command_buffer_free(&recorded.bar);
	if (recorded.background.dwords)
		xsdk_gpu_command_buffer_free(&recorded.background);
	xsdk_gpu_free(bar_b);
	xsdk_gpu_free(bar_a);
	xsdk_gpu_free(squares_vertices);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("replay: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
