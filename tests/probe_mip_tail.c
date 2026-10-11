#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "texture_shaders.h"
#include "tiled_index.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Where the GPU reads the mips of textures 16 texels or smaller across, from pages whose every texel names itself.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	FRAMES = 2,
	CLEAR_ARGB = 0xFF202020,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	// The base page, the page after it for the mips, and two more to catch reads past them.
	PAGES = 4,
	PAGE_BYTES = 0x1000,
	PAGE_TEXELS = 32,
	MAX_LEVELS = 6,
	MAX_CELLS = 96,
	ROW_HEIGHT = 30,
	LEVEL_GAP = 12,
	// A coded texel's blue holds this over its page.
	MARK = 0xC0,
	// The fetch constant's base and mip addresses (xenia:src/xenia/gpu/xenos.h:1230, 1282).
	ADDRESS_MASK = 0xFFFFF000,
};

typedef struct {
	uint32_t width;
	uint32_t height;
	uint32_t levels;
} Shape;

// Each short side 16 or less, so the packed tail starts at the base (xenia:src/xenia/gpu/texture_util.h:41-44).
static const Shape shapes[] = {
	{ 16, 16, 5 },
	{ 8, 8, 4 },
	{ 4, 4, 3 },
	{ 16, 8, 5 },
	{ 8, 16, 5 },
	{ 32, 16, 6 },
};

enum {
	SHAPES = sizeof shapes / sizeof shapes[0],
	// The mips on the page after the base's, as the SDK puts them, then on the base's own page.
	SETUPS = 2,
};

static const char *const setup_names[SETUPS] = { "mips on their own page", "mips on the base's page" };

typedef struct {
	uint32_t texture;
	uint32_t level;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
} Cell;

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

// Red holds x and green y, eight steps apart, and blue the mark and the page.
static uint32_t code(uint32_t page, uint32_t x, uint32_t y)
{
	return 0xFF000000 | x << 3 << 16 | y << 3 << 8 | MARK | page;
}

// Every texel of every page, as a 32 by 32 tile, laid out as resolves lay out a 32-bit target.
static void fill_pages(volatile uint32_t *pages)
{
	for (uint32_t page = 0; page < PAGES; page++)
		for (uint32_t y = 0; y < PAGE_TEXELS; y++)
			for (uint32_t x = 0; x < PAGE_TEXELS; x++)
				pages[page * PAGE_BYTES / 4 + tiled_index(x, y, PAGE_TEXELS)] = code(page, x, y);
	__asm__ volatile("sync" ::: "memory");
}

// The SDK's fetch constant for the shape, pointed at the coded pages.
static bool make_texture(XsdkTexture *texture, const Shape *shape, uint32_t setup, volatile uint32_t *pages)
{
	XsdkTexture model;
	if (!NT_SUCCESS(
			xsdk_texture_create(&model, XSDK_FORMAT_A8R8G8B8, shape->width, shape->height, shape->levels, true)))
		return false;
	const uint32_t base = xsdk_gpu_address(pages);
	const uint32_t mips = setup == 0 ? base + PAGE_BYTES : base;
	*texture = model;
	texture->texels = pages;
	texture->address = base;
	texture->size_bytes = PAGES * PAGE_BYTES;
	texture->fetch[1] = (model.fetch[1] & ~ADDRESS_MASK) | base;
	texture->fetch[5] = (model.fetch[5] & ~ADDRESS_MASK) | mips;
	xsdk_texture_free(&model);
	return true;
}

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
	const float right = clip_x(cell->x + cell->width - 0.5f);
	const float top = clip_y(cell->y - 0.5f);
	const float bottom = clip_y(cell->y + cell->height - 0.5f);
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

// One row per texture, its levels left to right at one texel to a pixel.
static uint32_t make_cells(Cell *cells)
{
	uint32_t count = 0;
	for (uint32_t texture = 0; texture < SHAPES * SETUPS; texture++) {
		const Shape *const shape = &shapes[texture % SHAPES];
		uint32_t x = 40;
		for (uint32_t level = 0; level < shape->levels; level++) {
			const uint32_t width = level_size(shape->width, level);
			const uint32_t height = level_size(shape->height, level);
			cells[count++] = (Cell){ texture, level, x, 40 + texture * ROW_HEIGHT, width, height };
			x += width + LEVEL_GAP;
		}
	}
	return count;
}

// The stored pixel as the texel it was drawn from: a resolve keeps the bytes B, G, R, A.
static uint32_t drawn_texel(volatile const uint32_t *pixels, uint32_t x, uint32_t y)
{
	return __builtin_bswap32(pixels[tiled_index(x, y, WIDTH)]);
}

static void describe(char *out, uint32_t size, uint32_t texel)
{
	if ((texel & 0xF0) != MARK) {
		snprintf(out, size, "uncoded %08X", (unsigned)texel);
		return;
	}
	snprintf(out, size, "page %u (%u, %u)", (unsigned)(texel & 0x0F), (unsigned)(texel >> 19 & 0x1F),
		(unsigned)(texel >> 11 & 0x1F));
}

// Whether the level reads one rectangle of one page, and where its corners came from.
static void report(const Cell *cell, volatile const uint32_t *pixels)
{
	const Shape *const shape = &shapes[cell->texture % SHAPES];
	const uint32_t first = drawn_texel(pixels, cell->x, cell->y);
	bool rectangle = (first & 0xF0) == MARK;
	for (uint32_t j = 0; j < cell->height && rectangle; j++) {
		for (uint32_t i = 0; i < cell->width && rectangle; i++) {
			const uint32_t texel = drawn_texel(pixels, cell->x + i, cell->y + j);
			rectangle = (texel & 0xFF) == (first & 0xFF) && (texel >> 19 & 0x1F) == (first >> 19 & 0x1F) + i &&
				(texel >> 11 & 0x1F) == (first >> 11 & 0x1F) + j;
		}
	}
	char corners[4][32];
	describe(corners[0], sizeof corners[0], first);
	describe(corners[1], sizeof corners[1], drawn_texel(pixels, cell->x + cell->width - 1, cell->y));
	describe(corners[2], sizeof corners[2], drawn_texel(pixels, cell->x, cell->y + cell->height - 1));
	describe(corners[3], sizeof corners[3], drawn_texel(pixels, cell->x + cell->width - 1, cell->y + cell->height - 1));
	xsdk_log_write("mip tail probe: %ux%u, %s, level %u (%ux%u): %s; corners %s, %s, %s, %s", (unsigned)shape->width,
		(unsigned)shape->height, setup_names[cell->texture / SHAPES], (unsigned)cell->level, (unsigned)cell->width,
		(unsigned)cell->height, rectangle ? "one rectangle" : "not one rectangle", corners[0], corners[1], corners[2],
		corners[3]);
}

int main(void)
{
	xsdk_log_write("mip tail probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("mip tail probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool ready = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, vertex_layout, 2, STRIDE_BYTES));
	static Cell cells[MAX_CELLS];
	const uint32_t cell_count = make_cells(cells);
	static XsdkTexture textures[SHAPES * SETUPS];
	bool made = ready;
	for (uint32_t i = 0; i < SHAPES * SETUPS && made; i++) {
		volatile uint32_t *const pages = xsdk_gpu_alloc(PAGES * PAGE_BYTES);
		made = pages && make_texture(&textures[i], &shapes[i % SHAPES], i / SHAPES, pages);
		if (made)
			fill_pages(pages);
	}
	volatile uint32_t *const vertices = xsdk_gpu_alloc(cell_count * QUAD_BYTES);
	volatile uint32_t *const buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!made || !vertices || !buffer) {
		xsdk_log_write("mip tail probe: setup failed");
		return 1;
	}
	for (uint32_t i = 0; i < cell_count; i++)
		put_quad(vertices + i * QUAD_BYTES / 4, &cells[i]);
	__asm__ volatile("sync" ::: "memory");
	const XsdkSampler point_clamped = { XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_ADDRESS_CLAMP,
		XSDK_ADDRESS_CLAMP, 0.0f };
	XsdkSamplerState sampler;
	xsdk_texture_sampler_state(&sampler, &point_clamped);
	bool landed = true;
	for (uint32_t frame = 0; frame < FRAMES && landed; frame++) {
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(&vertex, &pixel);
		for (uint32_t i = 0; i < cell_count; i++) {
			xsdk_gpu_set_vertices(xsdk_gpu_address(vertices) + i * QUAD_BYTES, QUAD_BYTES);
			xsdk_gpu_set_texture(TEXTURE_SLOT, &textures[cells[i].texture], &sampler);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		}
		xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
		landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	}
	xsdk_log_write("mip tail probe: fences %s", landed ? "landed" : "didn't land");
	if (landed)
		for (uint32_t i = 0; i < cell_count; i++)
			report(&cells[i], buffer);
	xsdk_log_write("mip tail probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
