#include <math.h>
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

// Every mip level drawn one texel to a pixel, then a cell per filter and address mode, checked against a CPU sampler.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXCOORD_OFFSET_BYTES = 16,
	TEXTURE_SLOT = 0,
	// Linear filters weigh texels in fixed point and round on the way out.
	LINEAR_TOLERANCE = 3,
	MAX_CELLS = 64,
	POINTS_PER_CELL = 10,
	MAX_LEVEL_TEXELS = 256 * 256,
	LEVEL_GAP = 12,
	CELL_SIZE = 128,
	HUES = 9,
	REGIONS = 4,
};

enum {
	GROUP_MIPS,
	GROUP_FILTERS,
	GROUP_ADDRESSING,
	GROUPS,
};

enum {
	TEXTURE_SQUARE,
	TEXTURE_WIDE,
	TEXTURE_TALL,
	TEXTURE_SMALL,
	TEXTURE_SMALL_LINEAR,
	TEXTURE_SMALL_WIDE,
	TEXTURE_TAIL_32,
	TEXTURE_TAIL_24,
	TEXTURE_CHECKER_SMALL,
	TEXTURE_CHECKER_LARGE,
	TEXTURE_SOLID_LEVELS,
	TEXTURE_BLOCKS,
	TEXTURES,
};

enum {
	// Each level its own hue, each texel its place in the low bits.
	PATTERN_LEVELS,
	// Each level one colour.
	PATTERN_SOLID,
	// Single-texel checks in 4 by 4 regions of colour.
	PATTERN_CHECKER,
	// 4 by 4 blocks of colour.
	PATTERN_BLOCKS,
};

typedef struct {
	const char *name;
	uint32_t width;
	uint32_t height;
	uint32_t levels;
	bool tiled;
	uint32_t pattern;
} TextureSpec;

// The mip rows cover a square, sides not a power of two each way, and bases inside the packed tail with their mips.
static const TextureSpec specs[TEXTURES] = {
	[TEXTURE_SQUARE] = { "128x128", 128, 128, 8, true, PATTERN_LEVELS },
	[TEXTURE_WIDE] = { "136x68", 136, 68, 8, true, PATTERN_LEVELS },
	[TEXTURE_TALL] = { "68x136", 68, 136, 8, true, PATTERN_LEVELS },
	[TEXTURE_SMALL] = { "16x16", 16, 16, 5, true, PATTERN_LEVELS },
	[TEXTURE_SMALL_LINEAR] = { "16x16 linear", 16, 16, 1, false, PATTERN_LEVELS },
	[TEXTURE_SMALL_WIDE] = { "16x8", 16, 8, 5, true, PATTERN_LEVELS },
	// From 17 to 32 across, the base isn't packed but level 1 starts the tail (texture_util.h:91-94).
	[TEXTURE_TAIL_32] = { "32x32", 32, 32, 6, true, PATTERN_LEVELS },
	[TEXTURE_TAIL_24] = { "24x20", 24, 20, 5, true, PATTERN_LEVELS },
	[TEXTURE_CHECKER_SMALL] = { "32x32 checker", 32, 32, 1, true, PATTERN_CHECKER },
	[TEXTURE_CHECKER_LARGE] = { "256x256 checker", 256, 256, 1, true, PATTERN_CHECKER },
	[TEXTURE_SOLID_LEVELS] = { "256x256 levels", 256, 256, 9, true, PATTERN_SOLID },
	[TEXTURE_BLOCKS] = { "32x32 blocks", 32, 32, 1, true, PATTERN_BLOCKS },
};

// Low five bits clear, so a texel's place fits under them.
static const uint32_t hues[HUES] = { 0xE02020, 0x20C020, 0x2040E0, 0xE0E020, 0xE020E0, 0x20E0E0, 0xE0E0E0, 0xA06020,
	0x6020A0 };

static const uint32_t region_colours[REGIONS * REGIONS] = { 0xE04040, 0x40E040, 0x4040E0, 0xE0E040, 0xE040E0, 0x40E0E0,
	0xE0E0E0, 0x606060, 0xE08040, 0x80E040, 0x4080E0, 0xE04080, 0x8040E0, 0x40E080, 0xA0A0A0, 0x806040 };

typedef struct {
	const char *name;
	uint32_t group;
	uint32_t texture;
	// The level a mip row's cell shows.
	uint32_t level;
	uint32_t x;
	uint32_t y;
	uint32_t width;
	uint32_t height;
	// Texture coordinates at the cell's left, top, right and bottom edges.
	float u0;
	float v0;
	float u1;
	float v1;
	XsdkSampler sampler;
} Cell;

typedef struct {
	uint32_t cell;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
	uint32_t tolerance;
} Point;

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

static const XsdkSampler point_clamped = { XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_ADDRESS_CLAMP,
	XSDK_ADDRESS_CLAMP, 0.0f, 0 };

static uint32_t image[MAX_LEVEL_TEXELS];

static uint32_t level_size(uint32_t size, uint32_t level)
{
	return size >> level ? size >> level : 1;
}

static uint32_t texel_argb(uint32_t texture, uint32_t level, uint32_t x, uint32_t y)
{
	const TextureSpec *const spec = &specs[texture];
	if (spec->pattern == PATTERN_LEVELS) {
		// Red and green's low bits hold x and y within 32, blue's which 32 they're in.
		const uint32_t place = (x & 31) << 16 | (y & 31) << 8 | ((x >> 5) + (y >> 5) * 5);
		return 0xFF000000 | hues[(level + texture) % HUES] | place;
	}
	if (spec->pattern == PATTERN_SOLID)
		return 0xFF000000 | hues[level % HUES];
	const uint32_t colour = region_colours[y * REGIONS / spec->height * REGIONS + x * REGIONS / spec->width];
	if (spec->pattern == PATTERN_BLOCKS)
		return 0xFF000000 | colour;
	return 0xFF000000 | ((x + y) & 1 ? colour : colour >> 2 & 0x3F3F3F);
}

static bool make_texture(XsdkTexture *texture, uint32_t which)
{
	const TextureSpec *const spec = &specs[which];
	const NTSTATUS status =
		xsdk_texture_create(texture, XSDK_FORMAT_A8R8G8B8, spec->width, spec->height, spec->levels, spec->tiled);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("sampling: %s: xsdk_texture_create failed: 0x%08X", spec->name, (unsigned)status);
		return false;
	}
	for (uint32_t level = 0; level < spec->levels; level++) {
		const uint32_t width = level_size(spec->width, level);
		const uint32_t height = level_size(spec->height, level);
		for (uint32_t y = 0; y < height; y++)
			for (uint32_t x = 0; x < width; x++)
				image[y * width + x] = texel_argb(which, level, x, y);
		xsdk_texture_upload(texture, level, image, width * sizeof image[0]);
		xsdk_gpu_invalidate(texture->address, texture->size_bytes);
	}
	return true;
}

// One cell per level, left to right, top edges level.
static uint32_t add_mip_row(Cell *cells, uint32_t count, uint32_t texture, uint32_t x, uint32_t y)
{
	const TextureSpec *const spec = &specs[texture];
	for (uint32_t level = 0; level < spec->levels; level++) {
		const uint32_t width = level_size(spec->width, level);
		const uint32_t height = level_size(spec->height, level);
		cells[count++] = (Cell){ spec->name, GROUP_MIPS, texture, level, x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f,
			point_clamped };
		x += width + LEVEL_GAP;
	}
	return count;
}

// Columns at x 480, 660, 840 and 1020, rows at y 40, 240 and 440.
static uint32_t add_grid(Cell *cells, uint32_t count)
{
	enum { POINT = XSDK_FILTER_POINT, LINEAR = XSDK_FILTER_LINEAR };
	enum { WRAP = XSDK_ADDRESS_WRAP, MIRROR = XSDK_ADDRESS_MIRROR, CLAMP = XSDK_ADDRESS_CLAMP };
	// A quarter texel in: point samples miss texel edges, and linear weighs a quarter and three quarters.
	const float in = 0.25f / 256.0f;
	const Cell grid[] = {
		// Four pixels a texel; the other filter is set the other way, so using it fails.
		{ "magnified, point", GROUP_FILTERS, TEXTURE_CHECKER_SMALL, 0, 480, 40, CELL_SIZE, CELL_SIZE, 0.0f, 0.0f, 1.0f,
			1.0f, { POINT, LINEAR, POINT, CLAMP, CLAMP, 0.0f, 0 } },
		{ "magnified, linear", GROUP_FILTERS, TEXTURE_CHECKER_SMALL, 0, 660, 40, CELL_SIZE, CELL_SIZE, 0.0f, 0.0f, 1.0f,
			1.0f, { LINEAR, POINT, POINT, CLAMP, CLAMP, 0.0f, 0 } },
		// Two texels a pixel.
		{ "minified, point", GROUP_FILTERS, TEXTURE_CHECKER_LARGE, 0, 840, 40, CELL_SIZE, CELL_SIZE, in, in, 1.0f + in,
			1.0f + in, { LINEAR, POINT, POINT, CLAMP, CLAMP, 0.0f, 0 } },
		{ "minified, linear", GROUP_FILTERS, TEXTURE_CHECKER_LARGE, 0, 1020, 40, CELL_SIZE, CELL_SIZE, in, in,
			1.0f + in, 1.0f + in, { POINT, LINEAR, POINT, CLAMP, CLAMP, 0.0f, 0 } },
		// Level 1, biased to 1.375: the nearest level is 1, and linear takes 3/8 of level 2.
		{ "mip point", GROUP_FILTERS, TEXTURE_SOLID_LEVELS, 0, 480, 240, CELL_SIZE, CELL_SIZE, 0.0f, 0.0f, 1.0f, 1.0f,
			{ POINT, POINT, POINT, CLAMP, CLAMP, 0.375f, 0 } },
		{ "mip linear", GROUP_FILTERS, TEXTURE_SOLID_LEVELS, 0, 660, 240, CELL_SIZE, CELL_SIZE, 0.0f, 0.0f, 1.0f, 1.0f,
			{ POINT, POINT, LINEAR, CLAMP, CLAMP, 0.375f, 0 } },
		// Half the texture past each edge, two pixels a texel.
		{ "wrap", GROUP_ADDRESSING, TEXTURE_BLOCKS, 0, 480, 440, CELL_SIZE, CELL_SIZE, -0.5f, -0.5f, 1.5f, 1.5f,
			{ POINT, POINT, POINT, WRAP, WRAP, 0.0f, 0 } },
		{ "mirror", GROUP_ADDRESSING, TEXTURE_BLOCKS, 0, 660, 440, CELL_SIZE, CELL_SIZE, -0.5f, -0.5f, 1.5f, 1.5f,
			{ POINT, POINT, POINT, MIRROR, MIRROR, 0.0f, 0 } },
		{ "clamp", GROUP_ADDRESSING, TEXTURE_BLOCKS, 0, 840, 440, CELL_SIZE, CELL_SIZE, -0.5f, -0.5f, 1.5f, 1.5f,
			{ POINT, POINT, POINT, CLAMP, CLAMP, 0.0f, 0 } },
		{ "mirror across, clamp down", GROUP_ADDRESSING, TEXTURE_BLOCKS, 0, 1020, 440, CELL_SIZE, CELL_SIZE, -0.5f,
			-0.5f, 1.5f, 1.5f, { POINT, POINT, POINT, MIRROR, CLAMP, 0.0f, 0 } },
	};
	for (uint32_t i = 0; i < sizeof grid / sizeof grid[0]; i++)
		cells[count++] = grid[i];
	return count;
}

static uint32_t make_cells(Cell *cells)
{
	uint32_t count = add_mip_row(cells, 0, TEXTURE_SQUARE, 40, 40);
	count = add_mip_row(cells, count, TEXTURE_WIDE, 40, 200);
	count = add_mip_row(cells, count, TEXTURE_TALL, 40, 300);
	count = add_mip_row(cells, count, TEXTURE_SMALL, 40, 470);
	count = add_mip_row(cells, count, TEXTURE_SMALL_LINEAR, 140, 470);
	count = add_mip_row(cells, count, TEXTURE_SMALL_WIDE, 40, 500);
	count = add_mip_row(cells, count, TEXTURE_TAIL_32, 40, 530);
	count = add_mip_row(cells, count, TEXTURE_TAIL_24, 200, 530);
	return add_grid(cells, count);
}

static int32_t floor_to_int(float value)
{
	const int32_t truncated = (int32_t)value;
	return (float)truncated > value ? truncated - 1 : truncated;
}

static uint32_t addressed(uint32_t mode, int32_t index, uint32_t size)
{
	if (mode == XSDK_ADDRESS_CLAMP)
		return index < 0 ? 0 : index >= (int32_t)size ? size - 1 : (uint32_t)index;
	const int32_t period = mode == XSDK_ADDRESS_MIRROR ? 2 * (int32_t)size : (int32_t)size;
	const int32_t folded = (index % period + period) % period;
	return (uint32_t)(folded < (int32_t)size ? folded : period - 1 - folded);
}

static void add_texel(float sum[3], const Cell *cell, uint32_t level, int32_t x, int32_t y, float weight)
{
	const TextureSpec *const spec = &specs[cell->texture];
	const uint32_t argb =
		texel_argb(cell->texture, level, addressed(cell->sampler.address_u, x, level_size(spec->width, level)),
			addressed(cell->sampler.address_v, y, level_size(spec->height, level)));
	for (uint32_t channel = 0; channel < 3; channel++)
		sum[channel] += weight * (float)(argb >> (16 - 8 * channel) & 0xFF);
}

static void sample_level(
	float sum[3], const Cell *cell, uint32_t level, uint32_t filter, float u, float v, float weight)
{
	const TextureSpec *const spec = &specs[cell->texture];
	const float x = u * (float)level_size(spec->width, level);
	const float y = v * (float)level_size(spec->height, level);
	if (filter == XSDK_FILTER_POINT) {
		add_texel(sum, cell, level, floor_to_int(x), floor_to_int(y), weight);
		return;
	}
	// The four texels whose centres surround the point.
	const int32_t left = floor_to_int(x - 0.5f);
	const int32_t top = floor_to_int(y - 0.5f);
	const float right_share = x - 0.5f - (float)left;
	const float bottom_share = y - 0.5f - (float)top;
	add_texel(sum, cell, level, left, top, weight * (1.0f - right_share) * (1.0f - bottom_share));
	add_texel(sum, cell, level, left + 1, top, weight * right_share * (1.0f - bottom_share));
	add_texel(sum, cell, level, left, top + 1, weight * (1.0f - right_share) * bottom_share);
	add_texel(sum, cell, level, left + 1, top + 1, weight * right_share * bottom_share);
}

// Texels a pixel steps over, as a power of two, plus the bias.
static float cell_lod(const Cell *cell)
{
	const TextureSpec *const spec = &specs[cell->texture];
	const float across = (cell->u1 - cell->u0) * (float)spec->width / (float)cell->width;
	const float down = (cell->v1 - cell->v0) * (float)spec->height / (float)cell->height;
	return log2f(across > down ? across : down) + cell->sampler.lod_bias;
}

static uint32_t cell_tolerance(const Cell *cell, float lod)
{
	const XsdkSampler *const sampler = &cell->sampler;
	const bool linear = lod <= 0.0f
		? sampler->mag_filter == XSDK_FILTER_LINEAR
		: sampler->min_filter == XSDK_FILTER_LINEAR || sampler->mip_filter == XSDK_FILTER_LINEAR;
	return linear ? LINEAR_TOLERANCE : 0;
}

static uint32_t to_byte(float value)
{
	const float rounded = value + 0.5f;
	return rounded < 0.0f ? 0 : rounded >= 255.0f ? 255 : (uint32_t)rounded;
}

// Magnified below level 0, else the nearest level or a blend of the two either side.
static uint32_t expected_argb(const Cell *cell, float lod, uint32_t x, uint32_t y)
{
	const XsdkSampler *const sampler = &cell->sampler;
	const float u = cell->u0 + (cell->u1 - cell->u0) * ((float)(x - cell->x) + 0.5f) / (float)cell->width;
	const float v = cell->v0 + (cell->v1 - cell->v0) * ((float)(y - cell->y) + 0.5f) / (float)cell->height;
	const uint32_t last = specs[cell->texture].levels - 1;
	float sum[3] = { 0.0f, 0.0f, 0.0f };
	if (lod <= 0.0f) {
		sample_level(sum, cell, 0, sampler->mag_filter, u, v, 1.0f);
	} else if (sampler->mip_filter == XSDK_FILTER_POINT) {
		const uint32_t nearest = (uint32_t)floor_to_int(lod + 0.5f);
		sample_level(sum, cell, nearest < last ? nearest : last, sampler->min_filter, u, v, 1.0f);
	} else {
		const uint32_t below = (uint32_t)floor_to_int(lod);
		const float above_share = below >= last ? 0.0f : lod - (float)below;
		sample_level(sum, cell, below < last ? below : last, sampler->min_filter, u, v, 1.0f - above_share);
		if (above_share > 0.0f)
			sample_level(sum, cell, below + 1, sampler->min_filter, u, v, above_share);
	}
	return 0xFF000000 | to_byte(sum[0]) << 16 | to_byte(sum[1]) << 8 | to_byte(sum[2]);
}

// Corners, middle and quarter points inside each cell, and the pixel past its bottom right.
static uint32_t make_points(const Cell *cells, uint32_t cell_count, Point *points)
{
	static const uint32_t quarters[POINTS_PER_CELL - 1][2] = { { 0, 0 }, { 4, 0 }, { 0, 4 }, { 4, 4 }, { 2, 2 },
		{ 1, 1 }, { 3, 1 }, { 1, 3 }, { 3, 3 } };
	uint32_t count = 0;
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		const float lod = cell_lod(cell);
		for (uint32_t j = 0; j < POINTS_PER_CELL - 1; j++) {
			const uint32_t x = cell->x + (cell->width - 1) * quarters[j][0] / 4;
			const uint32_t y = cell->y + (cell->height - 1) * quarters[j][1] / 4;
			points[count++] = (Point){ i, x, y, expected_argb(cell, lod, x, y), cell_tolerance(cell, lod) };
		}
		points[count++] = (Point){ i, cell->x + cell->width, cell->y + cell->height, CLEAR_ARGB, 0 };
	}
	return count;
}

// A resolve stores the colour's bytes B, G, R, A; alpha isn't checked.
static bool holds(uint32_t stored, uint32_t argb, uint32_t tolerance)
{
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > tolerance)
			return false;
	}
	return true;
}

static void log_miss(const Cell *cell, uint32_t frame, uint32_t x, uint32_t y, uint32_t stored, uint32_t argb)
{
	if (cell->group == GROUP_MIPS)
		xsdk_log_write("sampling: frame %u: %s level %u at (%u, %u) held %08X, not %08X", (unsigned)frame, cell->name,
			(unsigned)cell->level, (unsigned)x, (unsigned)y, (unsigned)stored, (unsigned)__builtin_bswap32(argb));
	else
		xsdk_log_write("sampling: frame %u: %s at (%u, %u) held %08X, not %08X", (unsigned)frame, cell->name,
			(unsigned)x, (unsigned)y, (unsigned)stored, (unsigned)__builtin_bswap32(argb));
}

// Every pixel of the cell and the ring around it; returns how many are wrong.
static uint32_t cell_wrong(const Cell *cell, volatile const uint32_t *pixels, uint32_t frame, uint32_t *logged)
{
	const float lod = cell_lod(cell);
	const uint32_t tolerance = cell_tolerance(cell, lod);
	uint32_t wrong = 0;
	for (uint32_t y = cell->y - 1; y <= cell->y + cell->height; y++) {
		for (uint32_t x = cell->x - 1; x <= cell->x + cell->width; x++) {
			const bool inside = x >= cell->x && x < cell->x + cell->width && y >= cell->y && y < cell->y + cell->height;
			const uint32_t argb = inside ? expected_argb(cell, lod, x, y) : CLEAR_ARGB;
			const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
			if (holds(stored, argb, inside ? tolerance : 0))
				continue;
			if ((*logged)++ < MAX_MISSES_LOGGED)
				log_miss(cell, frame, x, y, stored, argb);
			wrong++;
		}
	}
	return wrong;
}

// Adds each group's wrong pixels, naming each cell with any; returns how many pixels were checked.
static uint32_t count_wrong(
	const Cell *cells, uint32_t cell_count, volatile const uint32_t *pixels, uint32_t frame, uint32_t wrong[GROUPS])
{
	uint32_t checked = 0;
	uint32_t logged = 0;
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		const uint32_t ring = (cell->width + 2) * (cell->height + 2);
		const uint32_t cell_misses = cell_wrong(cell, pixels, frame, &logged);
		checked += ring;
		wrong[cell->group] += cell_misses;
		if (cell_misses && cell->group == GROUP_MIPS)
			xsdk_log_write("sampling: %s level %u: %u of %u pixels wrong", cell->name, (unsigned)cell->level,
				(unsigned)cell_misses, (unsigned)ring);
		else if (cell_misses)
			xsdk_log_write("sampling: %s: %u of %u pixels wrong", cell->name, (unsigned)cell_misses, (unsigned)ring);
	}
	return checked;
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
	const float right = clip_x(cell->x + cell->width - 0.5f);
	const float top = clip_y(cell->y - 0.5f);
	const float bottom = clip_y(cell->y + cell->height - 0.5f);
	const float quad[QUAD_VERTICES][VERTEX_FLOATS] = {
		{ left, top, 0.0f, 1.0f, cell->u0, cell->v0 },
		{ right, top, 0.0f, 1.0f, cell->u1, cell->v0 },
		{ left, bottom, 0.0f, 1.0f, cell->u0, cell->v1 },
		{ left, bottom, 0.0f, 1.0f, cell->u0, cell->v1 },
		{ right, top, 0.0f, 1.0f, cell->u1, cell->v0 },
		{ right, bottom, 0.0f, 1.0f, cell->u1, cell->v1 },
	};
	uint32_t words[sizeof quad / 4];
	memcpy(words, quad, sizeof quad);
	for (uint32_t i = 0; i < sizeof quad / 4; i++)
		vertices[i] = words[i];
}

typedef struct {
	const XsdkShader *vertex;
	const XsdkShader *pixel;
	const XsdkTexture *textures;
	const Cell *cells;
	const XsdkSamplerState *samplers;
	uint32_t cell_count;
	uint32_t vertices;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void draw_cells(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(scene->vertex, scene->pixel);
	for (uint32_t i = 0; i < scene->cell_count; i++) {
		xsdk_gpu_set_vertices(0, scene->vertices + i * QUAD_BYTES, QUAD_BYTES);
		xsdk_gpu_set_texture(TEXTURE_SLOT, &scene->textures[scene->cells[i].texture], &scene->samplers[i]);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	}
}

static const char *const group_checks[GROUPS] = {
	"sampling: every level of square, wide, tall and packed-base textures, one texel to a pixel",
	"sampling: magnification, minification and mip filters as worked out",
	"sampling: wrap, mirror and clamp as worked out",
};

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const Scene *scene)
{
	static Point points[MAX_CELLS * POINTS_PER_CELL];
	const uint32_t point_count = make_points(scene->cells, scene->cell_count, points);
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	uint32_t wrong[GROUPS] = { 0 };
	uint32_t checked = 0;
	uint32_t pixels_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		draw_cells(scene);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("sampling: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed) {
			bool frame_holds = true;
			for (uint32_t i = 0; i < point_count; i++) {
				const Point *const p = &points[i];
				const uint32_t stored = buffer[tiled_index(p->x, p->y, WIDTH)];
				if (holds(stored, p->argb, p->tolerance))
					continue;
				if (misses++ < MAX_MISSES_LOGGED)
					log_miss(&scene->cells[p->cell], frame, p->x, p->y, stored, p->argb);
				wrong[scene->cells[p->cell].group]++;
				frame_holds = false;
			}
			frames_wrong += !frame_holds;
		}
		if (frame == FRAMES - 1 && fences_landed) {
			uint32_t last_wrong[GROUPS] = { 0 };
			checked = count_wrong(scene->cells, scene->cell_count, buffer, frame, last_wrong);
			for (uint32_t i = 0; i < GROUPS; i++) {
				pixels_wrong += last_wrong[i];
				wrong[i] += last_wrong[i];
			}
		}
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("sampling: %u cells, %u points a frame; %u of %u frames checked held a wrong one, %u points wrong",
		(unsigned)scene->cell_count, (unsigned)point_count, (unsigned)frames_wrong, (unsigned)(FRAMES - 1),
		(unsigned)misses);
	xsdk_log_write("sampling: the last frame had %u of %u pixels wrong across the cells and their edges",
		(unsigned)pixels_wrong, (unsigned)checked);
	check("sampling: every frame's fence landed", fences_landed);
	for (uint32_t i = 0; i < GROUPS; i++)
		check(group_checks[i], fences_landed && checked > 0 && wrong[i] == 0);
	if (scene->has_display) {
		check("sampling: every frame shown", shown);
		xsdk_log_write("sampling: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

void test_sampling(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("sampling: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("sampling: display open at 1280x720", has_display);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = NT_SUCCESS(xsdk_shader_load(&vertex, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, texture_pixel_shader, sizeof texture_pixel_shader));
	check("sampling: shaders load", loaded);
	const bool patched = loaded &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, vertex_layout, sizeof vertex_layout / sizeof vertex_layout[0],
			(const uint32_t[]){ STRIDE_BYTES }));
	check("sampling: vertex fetches patched for a float4 position and float2 coordinates", patched);
	static XsdkTexture textures[TEXTURES];
	bool made = true;
	for (uint32_t i = 0; i < TEXTURES; i++)
		made = make_texture(&textures[i], i) && made;
	check("sampling: textures made with their levels", made);
	static Cell cells[MAX_CELLS];
	const uint32_t cell_count = make_cells(cells);
	static XsdkSamplerState samplers[MAX_CELLS];
	for (uint32_t i = 0; i < cell_count; i++)
		xsdk_texture_sampler_state(&samplers[i], &cells[i].sampler);
	volatile uint32_t *const vertices = xsdk_gpu_alloc(cell_count * QUAD_BYTES);
	if (vertices) {
		for (uint32_t i = 0; i < cell_count; i++)
			put_quad(vertices + i * QUAD_BYTES / 4, &cells[i]);
		__asm__ volatile("sync" ::: "memory");
		xsdk_gpu_invalidate(xsdk_gpu_address(vertices), cell_count * QUAD_BYTES);
	}
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("sampling: vertex and front buffers", vertices && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (patched && made && vertices && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = &vertex,
			.pixel = &pixel,
			.textures = textures,
			.cells = cells,
			.samplers = samplers,
			.cell_count = cell_count,
			.vertices = xsdk_gpu_address(vertices),
			.buffers = buffers,
			.has_display = has_display };
		draw_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	xsdk_gpu_free(vertices);
	for (uint32_t i = 0; i < TEXTURES; i++)
		if (textures[i].texels)
			xsdk_texture_free(&textures[i]);
	xsdk_shader_free(&pixel);
	xsdk_shader_free(&vertex);
	check("sampling: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
