#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constant_shaders.h"
#include "microcode_shaders.h"
#include "tests.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// Shaders' microcode in shared pages (SCOPE.md, milestone 25): 200 pixel shaders, each reading a constant of its own
// (tools/gen_microcode_shaders.py), fill one page where they took a page each, and each draws its own tile.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 30,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	SHADERS = 200,
	COLUMNS = 20,
	TILE_WIDTH = WIDTH / COLUMNS,
	TILE_HEIGHT = 72,
	// Each tile is drawn this far inside its cell, and points this far inside the tile hold its colour.
	TILE_MARGIN = 4,
	EDGE_MARGIN = 2,
	CHANNEL_TOLERANCE = 1,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// constant_vertex_shader places the unit quad with c0 and c1 (tools/gen_constant_shaders.py).
	VERTEX_SCALE = 0,
	VERTEX_OFFSET = 1,
	VERTEX_CONSTANTS = 3,
	// MmQueryStatistics counts 4 KB pages, 16 to a 64 KiB one (xenia:src/xenia/kernel/xboxkrnl/xboxkrnl_memory.cc:496).
	KERNEL_PAGES_PER_PAGE = 16,
	// 200 blocks fill less than one shared page, which may start a second if another shader holds blocks already.
	MAX_SHARED_PAGES = 2,
};

static XsdkShader vertex_shader;
static XsdkShader pixel_shaders[SHADERS];
static uint32_t containers[SHADERS][sizeof microcode_pixel_shader / 4];

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

// Next to each other, shaders differ in red; a row apart, in green.
static uint32_t shader_argb(uint32_t k)
{
	const uint32_t red = 30 + k % COLUMNS * 11;
	const uint32_t green = 30 + k / COLUMNS * 22;
	const uint32_t blue = k % 2 ? 200 : 60;
	return 0xFF000000 | red << 16 | green << 8 | blue;
}

static uint32_t tile_x(uint32_t k)
{
	return k % COLUMNS * TILE_WIDTH + TILE_MARGIN;
}

static uint32_t tile_y(uint32_t k)
{
	return k / COLUMNS * TILE_HEIGHT + TILE_MARGIN;
}

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

// The title's physical pages, in the kernel's 4 KB pages.
static uint32_t physical_pages(void)
{
	MM_QUERY_STATISTICS_RESULT statistics = { .size = sizeof statistics };
	const NTSTATUS status = MmQueryStatistics(&statistics);
	if (!NT_SUCCESS(status))
		xsdk_log_write("microcode: MmQueryStatistics failed: 0x%08X", (unsigned)status);
	return statistics.title.physical_pages;
}

// Shader k reads constant k: the template with its export's source registers rewritten.
static bool load_shaders(void)
{
	bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&vertex_shader, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	const uint32_t fields = 0xFFu << MICROCODE_SRC1_SHIFT | 0xFFu << MICROCODE_SRC2_SHIFT;
	for (uint32_t k = 0; k < SHADERS; k++) {
		memcpy(containers[k], microcode_pixel_shader, sizeof microcode_pixel_shader);
		uint32_t *const source = &containers[k][MICROCODE_SOURCE_DWORD];
		*source = (*source & ~fields) | k << MICROCODE_SRC1_SHIFT | k << MICROCODE_SRC2_SHIFT;
		loaded &= NT_SUCCESS(xsdk_shader_load(&pixel_shaders[k], containers[k], sizeof containers[k]));
	}
	return loaded;
}

static void free_shaders(void)
{
	for (uint32_t k = 0; k < SHADERS; k++)
		xsdk_shader_free(&pixel_shaders[k]);
	xsdk_shader_free(&vertex_shader);
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

// Constant k is shader k's colour.
static void set_pixel_constants(void)
{
	static float colors[SHADERS][4];
	for (uint32_t k = 0; k < SHADERS; k++) {
		const uint32_t argb = shader_argb(k);
		const float color[4] = { (argb >> 16 & 0xFF) / 255.0f, (argb >> 8 & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
			1.0f };
		memcpy(colors[k], color, sizeof color);
	}
	xsdk_gpu_set_pixel_constants(0, &colors[0][0], SHADERS);
}

// Edges half a pixel early, as the other tests place theirs (xenia:src/xenia/gpu/draw_util.cc:837).
static void place_tile(uint32_t k)
{
	const uint32_t width = TILE_WIDTH - 2 * TILE_MARGIN;
	const uint32_t height = TILE_HEIGHT - 2 * TILE_MARGIN;
	const float left = (tile_x(k) - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (tile_x(k) + width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (tile_y(k) - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (tile_y(k) + height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[VERTEX_CONSTANTS][4] = {
		[VERTEX_SCALE] = { right - left, bottom - top, 1.0f, 1.0f },
		[VERTEX_OFFSET] = { left, top, 0.0f, 0.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], VERTEX_CONSTANTS);
}

// Tile k's centre and two points inside its corners hold shader k's colour.
static bool tile_holds(volatile const uint32_t *pixels, uint32_t frame, uint32_t k, uint32_t *misses)
{
	const uint32_t width = TILE_WIDTH - 2 * TILE_MARGIN;
	const uint32_t height = TILE_HEIGHT - 2 * TILE_MARGIN;
	const uint32_t points[3][2] = { { tile_x(k) + width / 2, tile_y(k) + height / 2 },
		{ tile_x(k) + EDGE_MARGIN, tile_y(k) + EDGE_MARGIN },
		{ tile_x(k) + width - 1 - EDGE_MARGIN, tile_y(k) + height - 1 - EDGE_MARGIN } };
	bool holds = true;
	for (uint32_t p = 0; p < 3; p++) {
		const uint32_t stored = pixels[tiled_index(points[p][0], points[p][1], WIDTH)];
		const bool match = channels_match(stored, as_stored(shader_argb(k)));
		if (!match && *misses < MAX_MISSES_LOGGED)
			xsdk_log_write("microcode: frame %u: shader %u's tile at (%u, %u) held %08X, not %08X", (unsigned)frame,
				(unsigned)k, (unsigned)points[p][0], (unsigned)points[p][1], (unsigned)stored,
				(unsigned)as_stored(shader_argb(k)));
		*misses += !match;
		holds &= match;
	}
	return holds;
}

static bool draw_frames(uint32_t vertices, volatile uint32_t *const *buffers, bool has_display)
{
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	uint32_t frames_checked = 0;
	bool landed = true;
	for (uint32_t frame = 0; frame < FRAMES && landed; frame++) {
		volatile uint32_t *const buffer = buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		set_pixel_constants();
		for (uint32_t k = 0; k < SHADERS; k++) {
			xsdk_gpu_set_shaders(&vertex_shader, &pixel_shaders[k]);
			place_tile(k);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		}
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!landed)
			xsdk_log_write("microcode: frame %u never finished", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && landed) {
			bool holds = true;
			for (uint32_t k = 0; k < SHADERS; k++)
				holds &= tile_holds(buffer, frame, k, &misses);
			frames_wrong += !holds;
			frames_checked++;
		}
		if (has_display)
			(void)xsdk_display_show(address, WIDTH);
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("microcode: %u of %u frames checked held a wrong tile; %u points wrong", (unsigned)frames_wrong,
		(unsigned)frames_checked, (unsigned)misses);
	return landed && frames_checked == FRAMES - 1 && frames_wrong == 0;
}

void test_microcode(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("microcode: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	volatile uint32_t *const vertices = upload_square();
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("microcode: vertex and front buffers", vertices && buffers[0] && buffers[1]);

	const uint32_t before = physical_pages();
	const bool loaded = load_shaders();
	const uint32_t loaded_pages = physical_pages() - before;
	check("microcode: 200 small pixel shaders and a vertex shader load", loaded);
	xsdk_log_write("microcode: the 201 shaders took %u of the kernel's 4 KB pages; a page each would be %u",
		(unsigned)loaded_pages, (unsigned)((SHADERS + 1) * KERNEL_PAGES_PER_PAGE));
	check("microcode: the shaders share pages, two at most", loaded_pages <= MAX_SHARED_PAGES * KERNEL_PAGES_PER_PAGE);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (loaded && vertices && buffers[0] && buffers[1])
		check("microcode: every frame draws each shader's tile in its own colour",
			draw_frames(xsdk_gpu_address(vertices), buffers, has_display));
	free_shaders();
	const uint32_t after = physical_pages();
	xsdk_log_write(
		"microcode: freed, the title holds %u pages, from %u before the shaders", (unsigned)after, (unsigned)before);
	check("microcode: freeing the shaders gives their pages back", after <= before);

	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	check("microcode: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
