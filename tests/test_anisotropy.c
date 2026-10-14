#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "texture_shaders.h"
#include "tiled_index.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"
#include "xsdk/thread.h"

// Thin vertical stripes on quads squeezed 8 to 1 down them, one for each most anisotropy, their contrast measured.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 4,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	TEXTURE_SIZE = 256,
	LEVELS = 9,
	// Two texels wide: level 1 halves their contrast, and level 2 and up average them to grey.
	STRIPE_TEXELS = 2,
	SQUEEZE = 8,
	// One texel a pixel across, eight down: without anisotropy the GPU picks level 3.
	CELL_WIDTH = TEXTURE_SIZE,
	CELL_HEIGHT = TEXTURE_SIZE / SQUEEZE,
	CELLS = 6,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	WHITE = 255,
	PER_MILLE = 1000,
	// Level 0 gives a contrast of 1000, level 1 500 and level 2 on 0: kept is level 0.5 or under, greyed 1.5 or over.
	KEPT_MIN_PER_MILLE = 750,
	GREYED_MAX_PER_MILLE = 250,
	// Grey is level 2's 128 of 255; a cell not drawn holds the clear colour's 32.
	GREY_LOW_PER_MILLE = 400,
	GREY_HIGH_PER_MILLE = 600,
};

typedef struct {
	uint32_t max_anisotropy;
	uint32_t x;
	uint32_t y;
} Cell;

typedef struct {
	int32_t contrast_per_mille;
	uint32_t mean_per_mille;
} Stripes;

// None, then each power of two.
static const Cell cells[CELLS] = {
	{ 0, 128, 160 },
	{ 1, 512, 160 },
	{ 2, 896, 160 },
	{ 4, 128, 400 },
	{ 8, 512, 400 },
	{ 16, 896, 400 },
};

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

static uint8_t levels[2][TEXTURE_SIZE * TEXTURE_SIZE];
static uint32_t image[TEXTURE_SIZE * TEXTURE_SIZE];

static bool white_column(uint32_t x)
{
	return x / STRIPE_TEXELS % 2 == 0;
}

// Each level the box-filtered average of the one above, as mips are made.
static void next_level(const uint8_t *above, uint8_t *below, uint32_t size)
{
	for (uint32_t y = 0; y < size; y++)
		for (uint32_t x = 0; x < size; x++) {
			const uint8_t *const quad = above + 2 * y * 2 * size + 2 * x;
			below[y * size + x] = (uint8_t)((quad[0] + quad[1] + quad[2 * size] + quad[2 * size + 1] + 2) / 4);
		}
}

static bool make_texture(XsdkTexture *texture)
{
	const NTSTATUS status =
		xsdk_texture_create(texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, LEVELS, true);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("anisotropy: xsdk_texture_create failed: 0x%08X", (unsigned)status);
		return false;
	}
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++)
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			levels[0][y * TEXTURE_SIZE + x] = white_column(x) ? WHITE : 0;
	for (uint32_t level = 0; level < LEVELS; level++) {
		const uint32_t size = TEXTURE_SIZE >> level;
		const uint8_t *const grey = levels[level % 2];
		for (uint32_t i = 0; i < size * size; i++)
			image[i] = 0xFF000000 | (uint32_t)grey[i] * 0x010101;
		xsdk_texture_upload(texture, level, image, size * sizeof image[0]);
		if (level + 1 < LEVELS)
			next_level(grey, levels[(level + 1) % 2], size / 2);
	}
	xsdk_gpu_invalidate(texture->address, texture->size_bytes);
	return true;
}

// A screen position in clip space, through the viewport xsdk_gpu_set_target sets.
static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// Two triangles, edges half a pixel before the pixels, for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837).
static void put_quad(volatile uint32_t *vertices, const Cell *cell)
{
	const float left = clip_x(cell->x - 0.5f);
	const float right = clip_x(cell->x + CELL_WIDTH - 0.5f);
	const float top = clip_y(cell->y - 0.5f);
	const float bottom = clip_y(cell->y + CELL_HEIGHT - 0.5f);
	const float quad[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, 0.0f, 0.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ left, bottom, 0.0f, 1.0f, 0.0f, 1.0f },
		{ right, top, 0.0f, 1.0f, 1.0f, 0.0f },
		{ right, bottom, 0.0f, 1.0f, 1.0f, 1.0f },
	};
	uint32_t words[sizeof quad / 4];
	memcpy(words, quad, sizeof quad);
	for (uint32_t i = 0; i < sizeof quad / 4; i++)
		vertices[i] = words[i];
}

// The white stripes' average less the black ones', and the average of both, in green, which a resolve stores second.
static Stripes measure(volatile const uint32_t *pixels, const Cell *cell)
{
	uint64_t sums[2] = { 0, 0 };
	uint64_t counts[2] = { 0, 0 };
	for (uint32_t y = cell->y; y < cell->y + CELL_HEIGHT; y++)
		for (uint32_t x = cell->x; x < cell->x + CELL_WIDTH; x++) {
			const bool white = white_column(x - cell->x);
			sums[white] += pixels[tiled_index(x, y, WIDTH)] >> 16 & 0xFF;
			counts[white]++;
		}
	const int64_t difference = (int64_t)(sums[1] * counts[0]) - (int64_t)(sums[0] * counts[1]);
	return (
		Stripes){ .contrast_per_mille = (int32_t)(difference * PER_MILLE / (int64_t)(counts[0] * counts[1] * WHITE)),
		.mean_per_mille = (uint32_t)((sums[0] + sums[1]) * PER_MILLE / ((counts[0] + counts[1]) * WHITE)) };
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	const XsdkTexture *texture;
	const XsdkSamplerState *samplers;
	uint32_t vertices;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void draw_cells(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	for (uint32_t i = 0; i < CELLS; i++) {
		xsdk_gpu_set_vertices(0, scene->vertices + i * QUAD_BYTES, QUAD_BYTES);
		xsdk_gpu_set_texture(TEXTURE_SLOT, scene->texture, &scene->samplers[i]);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

static void check_frames(const Scene *scene)
{
	bool fences_landed = true;
	uint32_t frame = 0;
	for (; frame < FRAMES && fences_landed; frame++) {
		draw_cells(scene);
		xsdk_gpu_resolve(xsdk_gpu_address(scene->buffers[frame % BUFFERS]), WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	}
	check("anisotropy: every frame's fence landed", fences_landed);
	if (!fences_landed)
		return;
	volatile uint32_t *const last = scene->buffers[(frame - 1) % BUFFERS];
	Stripes stripes[CELLS];
	for (uint32_t i = 0; i < CELLS; i++) {
		stripes[i] = measure(last, &cells[i]);
		xsdk_log_write("anisotropy: at most %u: stripes' contrast %d, mean %u, per mille",
			(unsigned)cells[i].max_anisotropy, (int)stripes[i].contrast_per_mille, (unsigned)stripes[i].mean_per_mille);
	}
	const Stripes *const none = &stripes[0];
	const Stripes *const most = &stripes[CELLS - 1];
	check("anisotropy: at most 16, the stripes keep their contrast",
		most->contrast_per_mille >= (int32_t)KEPT_MIN_PER_MILLE);
	check("anisotropy: without, the level the GPU picks greys the stripes",
		none->contrast_per_mille <= (int32_t)GREYED_MAX_PER_MILLE && none->mean_per_mille >= GREY_LOW_PER_MILLE &&
			none->mean_per_mille <= GREY_HIGH_PER_MILLE);
	if (!scene->has_display)
		return;
	check("anisotropy: the last frame shown", NT_SUCCESS(xsdk_display_show(xsdk_gpu_address(last), WIDTH)));
	xsdk_log_write("anisotropy: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
	(void)xsdk_thread_sleep(TEST_SHOW_MS);
}

void test_anisotropy(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("anisotropy: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0],
			(const uint32_t[]){ STRIDE_BYTES }));
	XsdkTexture texture = { .texels = NULL };
	const bool made = make_texture(&texture);
	XsdkSamplerState samplers[CELLS];
	for (uint32_t i = 0; i < CELLS; i++) {
		const XsdkSampler sampler = { .mag_filter = XSDK_FILTER_LINEAR,
			.min_filter = XSDK_FILTER_LINEAR,
			.mip_filter = XSDK_FILTER_LINEAR,
			.address_u = XSDK_ADDRESS_CLAMP,
			.address_v = XSDK_ADDRESS_CLAMP,
			.max_anisotropy = cells[i].max_anisotropy };
		xsdk_texture_sampler_state(&samplers[i], &sampler);
	}
	volatile uint32_t *const vertices = xsdk_gpu_alloc(CELLS * QUAD_BYTES);
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	const bool ready = loaded && made && vertices && buffers[0] && buffers[1];
	check("anisotropy: shaders, striped texture with its levels, vertex and front buffers", ready);
	if (ready) {
		for (uint32_t i = 0; i < CELLS; i++)
			put_quad(vertices + i * QUAD_BYTES / 4, &cells[i]);
		__asm__ volatile("sync" ::: "memory");
		xsdk_gpu_invalidate(xsdk_gpu_address(vertices), CELLS * QUAD_BYTES);
		// Without a display, as in Xenia, the frames still draw and are checked.
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.texture = &texture,
			.samplers = samplers,
			.vertices = xsdk_gpu_address(vertices),
			.buffers = buffers,
			.has_display = has_display };
		check_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	if (texture.texels)
		xsdk_texture_free(&texture);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("anisotropy: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
