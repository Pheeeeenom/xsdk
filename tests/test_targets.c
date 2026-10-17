#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "microcode_shaders.h"
#include "target_shaders.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Render target formats and four targets at once (vulkan4xenon milestone 25), each target resolved into a texture of
// the format it converts to and every texel read back checked by value.
// - Each format alone: a resolve's clear, a quad over the left half, and on the top of the right half the quad twice,
//   the second time blended onto the first, which doubles it where the format blends.
// - Four targets of four formats at once: one draw writes each its own colour over the left half, then one over the
//   top half blends and masks each its own way.

enum {
	TARGET_WIDTH = 64,
	TARGET_HEIGHT = 32,
	// The quad covers the columns left of this; the blended one, the rows above BLEND_BOTTOM right of it.
	QUAD_RIGHT = 32,
	BLEND_BOTTOM = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 4,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// constant_vertex_shader places the unit quad with c0 and c1 (tools/gen_constant_shaders.py).
	VERTEX_SCALE = 0,
	VERTEX_OFFSET = 1,
	VERTEX_CONSTANTS = 3,
	CHANNELS = 4,
	// The most bytes a texel takes: four half floats.
	MAX_TEXEL_BYTES = 8,
	LABEL_BYTES = 112,
};

// How a texture holds a texel.
enum {
	// One byte a channel, red first.
	KIND_BYTES,
	// One word: red, green and blue in 10 bits from the low end, alpha in the top 2.
	KIND_10_10_10_2,
	// One half float a channel, red first.
	KIND_HALVES,
	// One float, red.
	KIND_FLOATS,
};

typedef struct {
	uint32_t kind;
	uint32_t channels;
	// How far a channel may be from what's drawn: a unorm's step, or a fixed point's and a half's.
	float tolerance;
} Holding;

// The texture's channels, the clear and the quad's colour, each channel apart from the others, past 1 where the format
// holds it. Doubled, the colour stays within the format, so blending shows.
typedef struct {
	const char *name;
	uint32_t target;
	uint32_t texture;
	Holding holding;
	float clear[CHANNELS];
	float color[CHANNELS];
	// vulkan4xenon reports blending for it, so its blend is a check; the others' is logged.
	bool blend_checked;
} Case;

static const Case cases[] = {
	{ "2_10_10_10", XSDK_TARGET_2_10_10_10, XSDK_FORMAT_A2B10G10R10, { KIND_10_10_10_2, 4, 1.5f / 1023 },
		{ 0.2f, 0.4f, 0.6f, 1.0f }, { 0.45f, 0.35f, 0.05f, 1.0f / 3 }, true },
	{ "2_10_10_10_FLOAT", XSDK_TARGET_2_10_10_10_FLOAT, XSDK_FORMAT_R16G16B16A16F, { KIND_HALVES, 4, 0.002f },
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, 1.0f / 3 }, false },
	{ "16_16_16_16", XSDK_TARGET_16_16_16_16, XSDK_FORMAT_R16G16B16A16F, { KIND_HALVES, 4, 0.002f },
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, -0.5f }, false },
	{ "16_16_FLOAT", XSDK_TARGET_16_16_FLOAT, XSDK_FORMAT_R16G16F, { KIND_HALVES, 2, 0.0f }, { 0.25f, 0.5f },
		{ 1.5f, 1000.0f }, false },
	{ "16_16_16_16_FLOAT", XSDK_TARGET_16_16_16_16_FLOAT, XSDK_FORMAT_R16G16B16A16F, { KIND_HALVES, 4, 0.0f },
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 1000.0f, -2.0f }, false },
	{ "32_FLOAT", XSDK_TARGET_32_FLOAT, XSDK_FORMAT_R32F, { KIND_FLOATS, 1, 0.0f }, { 0.25f }, { 1234.5f }, false },
};

enum { CASES = sizeof cases / sizeof cases[0] };

// The quarters of a target the two draws of four targets leave apart.
enum {
	QUARTER_BOTH,
	QUARTER_CLEAR_SECOND,
	QUARTER_FIRST,
	QUARTER_CLEAR,
	QUARTERS,
};

// One of the four targets: its clear, the colours its oC takes in the first draw and the second, how the second blends
// and masks, and what each quarter then holds.
typedef struct {
	const char *what;
	uint32_t format;
	uint32_t texture;
	Holding holding;
	float clear[CHANNELS];
	float first[CHANNELS];
	float second[CHANNELS];
	XsdkBlend blend;
	float quarters[QUARTERS][CHANNELS];
} FourTarget;

static const FourTarget four[XSDK_COLOR_TARGETS] = {
	{ "8_8_8_8, oC0 added onto what it holds", XSDK_TARGET_8_8_8_8, XSDK_FORMAT_R8G8B8A8, { KIND_BYTES, 4, 1.5f / 255 },
		{ 0.2f, 0.4f, 0.6f, 0.8f }, { 0.8f, 0.6f, 0.0f, 1.0f }, { 0.2f, 0.2f, 0.4f, 0.0f },
		{ XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ONE, XSDK_BLEND_OP_ADD,
			XSDK_WRITE_ALL },
		{ { 1.0f, 0.8f, 0.4f, 1.0f }, { 0.4f, 0.6f, 1.0f, 0.8f }, { 0.8f, 0.6f, 0.0f, 1.0f },
			{ 0.2f, 0.4f, 0.6f, 0.8f } } },
	// The second draw's alpha is one 2 bits hold, as the console rounds the source to the target's bits before it
	// blends (docs/HARDWARE.md, "Float, 10-bit and 64-bit targets and four at once").
	{ "2_10_10_10, oC1 blended by its alpha, red, green and blue alone", XSDK_TARGET_2_10_10_10,
		XSDK_FORMAT_A2B10G10R10, { KIND_10_10_10_2, 4, 1.5f / 1023 }, { 0.2f, 0.4f, 0.6f, 1.0f / 3 },
		{ 0.8f, 0.6f, 0.4f, 1.0f }, { 0.0f, 1.0f, 0.2f, 1.0f / 3 },
		{ XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
			XSDK_BLEND_OP_ADD, XSDK_WRITE_RED | XSDK_WRITE_GREEN | XSDK_WRITE_BLUE },
		{ { 1.6f / 3, 2.2f / 3, 1.0f / 3, 1.0f }, { 0.4f / 3, 0.6f, 1.4f / 3, 1.0f / 3 }, { 0.8f, 0.6f, 0.4f, 1.0f },
			{ 0.2f, 0.4f, 0.6f, 1.0f / 3 } } },
	{ "16_16_16_16_FLOAT, oC2 past 1, red and alpha alone", XSDK_TARGET_16_16_16_16_FLOAT, XSDK_FORMAT_R16G16B16A16F,
		{ KIND_HALVES, 4, 0.0f }, { 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, 4.0f }, { 6.0f, 7.0f, 8.0f, 9.0f },
		{ XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD,
			XSDK_WRITE_RED | XSDK_WRITE_ALPHA },
		{ { 6.0f, 2.25f, 3.0f, 9.0f }, { 6.0f, 0.5f, 0.75f, 9.0f }, { 1.5f, 2.25f, 3.0f, 4.0f },
			{ 0.25f, 0.5f, 0.75f, 1.0f } } },
	{ "32_FLOAT, oC3 past 1, kept from the second draw", XSDK_TARGET_32_FLOAT, XSDK_FORMAT_R32F,
		{ KIND_FLOATS, 1, 0.0f }, { 0.125f }, { 5.5f }, { 10.0f },
		{ XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, 0 },
		{ { 5.5f }, { 0.125f }, { 5.5f }, { 0.125f } } },
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

static uint32_t readback[TARGET_WIDTH * TARGET_HEIGHT * MAX_TEXEL_BYTES / 4];

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	XsdkShader four_pixel;
	uint32_t vertices;
	XsdkBlendState opaque;
	XsdkBlendState additive;
} Scene;

static float half_value(uint32_t half)
{
	const uint32_t exponent = half >> 10 & 0x1F;
	const float mantissa = (float)(half & 0x3FF) / 1024.0f;
	const float magnitude = exponent ? ldexpf(1.0f + mantissa, (int)exponent - 15) : ldexpf(mantissa, -14);
	return half & 0x8000 ? -magnitude : magnitude;
}

// Channel c of texel (x, y) read back, as the texture's format holds it.
static float texel_channel(const Holding *holding, uint32_t x, uint32_t y, uint32_t c)
{
	const uint32_t index = y * TARGET_WIDTH + x;
	if (holding->kind == KIND_BYTES)
		return ((const uint8_t *)readback)[index * holding->channels + c] / 255.0f;
	if (holding->kind == KIND_10_10_10_2) {
		const uint32_t word = readback[index];
		return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
	}
	if (holding->kind == KIND_HALVES) {
		uint16_t half;
		memcpy(&half, (const uint8_t *)readback + (index * holding->channels + c) * sizeof half, sizeof half);
		return half_value(half);
	}
	float value;
	memcpy(&value, &readback[index * holding->channels + c], sizeof value);
	return value;
}

static bool texel_near(const Holding *holding, uint32_t x, uint32_t y, const float *want)
{
	// Written so a NaN, which a float target can hold, is never near.
	for (uint32_t c = 0; c < holding->channels; c++) {
		if (!(fabsf(texel_channel(holding, x, y, c) - want[c]) <= holding->tolerance))
			return false;
	}
	return true;
}

static void log_texel(
	const Holding *holding, const char *name, const char *what, uint32_t x, uint32_t y, const float *want)
{
	float got[CHANNELS] = { 0.0f };
	float wanted[CHANNELS] = { 0.0f };
	for (uint32_t c = 0; c < holding->channels; c++) {
		got[c] = texel_channel(holding, x, y, c);
		wanted[c] = want[c];
	}
	xsdk_log_write("targets: %s: %s at (%u, %u) is %g %g %g %g, not %g %g %g %g", name, what, (unsigned)x, (unsigned)y,
		got[0], got[1], got[2], got[3], wanted[0], wanted[1], wanted[2], wanted[3]);
}

static uint32_t texel_bytes(const Holding *holding)
{
	switch (holding->kind) {
	case KIND_BYTES:
		return holding->channels;
	case KIND_HALVES:
		return 2 * holding->channels;
	default:
		return 4;
	}
}

// Clip space for a rectangle of the target's pixels, edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
static void place_rectangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	const float left = (x0 - 0.5f) / (TARGET_WIDTH / 2.0f) - 1.0f;
	const float right = (x1 - 0.5f) / (TARGET_WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (y0 - 0.5f) / (TARGET_HEIGHT / 2.0f);
	const float bottom = 1.0f - (y1 - 0.5f) / (TARGET_HEIGHT / 2.0f);
	const float vertex[VERTEX_CONSTANTS][4] = {
		[VERTEX_SCALE] = { right - left, bottom - top, 1.0f, 1.0f },
		[VERTEX_OFFSET] = { left, top, 0.0f, 0.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], VERTEX_CONSTANTS);
}

static void draw_rectangle(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
	place_rectangle(x0, y0, x1, y1);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static bool finished(const char *what)
{
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	if (!landed)
		xsdk_log_write("targets: %s: the resolve never finished", what);
	return landed;
}

static uint32_t depth_tile(void)
{
	return XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(TARGET_WIDTH, TARGET_HEIGHT);
}

// Clears with a resolve, which copies what eDRAM held; draws the quad and the blended one; resolves again.
static bool draw_case(const Scene *scene, const Case *test, XsdkTexture *texture)
{
	const XsdkColorTarget target = { .format = test->target, .tile = 0 };
	float color[CHANNELS] = { 0.0f, 0.0f, 0.0f, 1.0f };
	memcpy(color, test->color, test->holding.channels * sizeof color[0]);
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, &target, 1, depth_tile());
	xsdk_gpu_resolve_target(0, texture, test->clear);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, &target, 1, depth_tile());
	xsdk_gpu_set_shaders(&scene->vertex, &scene->pixel);
	xsdk_gpu_set_vertices(0, scene->vertices, sizeof square);
	xsdk_gpu_set_pixel_constants(0, color, 1);
	xsdk_gpu_set_blend(&scene->opaque);
	draw_rectangle(0, 0, QUAD_RIGHT, TARGET_HEIGHT);
	draw_rectangle(QUAD_RIGHT, 0, TARGET_WIDTH, BLEND_BOTTOM);
	xsdk_gpu_set_blend(&scene->additive);
	draw_rectangle(QUAD_RIGHT, 0, TARGET_WIDTH, BLEND_BOTTOM);
	xsdk_gpu_resolve_target(0, texture, test->clear);
	xsdk_gpu_set_resolve_clears(true, true);
	return finished(test->name);
}

// The quad's colour left of QUAD_RIGHT and the clear below the blended rows right of it, every texel.
static uint32_t count_wrong(const Case *test)
{
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			if (x >= QUAD_RIGHT && y < BLEND_BOTTOM)
				continue;
			const bool quad = x < QUAD_RIGHT;
			const float *const want = quad ? test->color : test->clear;
			if (texel_near(&test->holding, x, y, want))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_texel(&test->holding, test->name, quad ? "the quad" : "the clear", x, y, want);
			wrong++;
		}
	}
	return wrong;
}

// The blended rows: the colour doubled where the format blends.
static uint32_t count_unblended(const Case *test, bool logged)
{
	float doubled[CHANNELS];
	for (uint32_t c = 0; c < test->holding.channels; c++)
		doubled[c] = 2.0f * test->color[c];
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < BLEND_BOTTOM; y++) {
		for (uint32_t x = QUAD_RIGHT; x < TARGET_WIDTH; x++) {
			if (texel_near(&test->holding, x, y, doubled))
				continue;
			if (logged && wrong < MAX_MISSES_LOGGED)
				log_texel(&test->holding, test->name, "the quad blended onto itself", x, y, doubled);
			wrong++;
		}
	}
	return wrong;
}

static void log_blend(const Case *test, uint32_t unblended)
{
	float got[CHANNELS] = { 0.0f };
	for (uint32_t c = 0; c < test->holding.channels; c++)
		got[c] = texel_channel(&test->holding, QUAD_RIGHT, 0, c);
	xsdk_log_write("targets: %s: a quad blended onto itself gave %g %g %g %g; %u of %u texels doubled", test->name,
		got[0], got[1], got[2], got[3], (unsigned)((TARGET_WIDTH - QUAD_RIGHT) * BLEND_BOTTOM - unblended),
		(unsigned)((TARGET_WIDTH - QUAD_RIGHT) * BLEND_BOTTOM));
}

static void check_case(const Scene *scene, const Case *test)
{
	XsdkTexture texture = { .texels = NULL };
	char name[LABEL_BYTES];
	const bool made = NT_SUCCESS(xsdk_texture_create(&texture, test->texture, TARGET_WIDTH, TARGET_HEIGHT, 1, true));
	const bool drawn = made && draw_case(scene, test, &texture);
	if (drawn)
		xsdk_texture_download_rows(&texture, 0, 0, TARGET_HEIGHT, readback, TARGET_WIDTH * texel_bytes(&test->holding));
	const uint32_t wrong = drawn ? count_wrong(test) : 0;
	snprintf(name, sizeof name, "targets: %s holds its clear and a quad's colour, each channel in place", test->name);
	check(name, drawn && wrong == 0);
	const uint32_t unblended = drawn ? count_unblended(test, test->blend_checked) : 0;
	if (test->blend_checked) {
		snprintf(name, sizeof name, "targets: %s blends a quad onto itself, doubling it", test->name);
		check(name, drawn && unblended == 0);
	} else if (drawn) {
		log_blend(test, unblended);
	}
	if (made)
		xsdk_texture_free(&texture);
}

// The targets one after another from tile 0, each as many tiles as its format takes.
static void place_four(XsdkColorTarget targets[XSDK_COLOR_TARGETS])
{
	uint32_t tile = 0;
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		targets[i] = (XsdkColorTarget){ .format = four[i].format, .tile = tile };
		tile += xsdk_gpu_color_target_tiles(TARGET_WIDTH, TARGET_HEIGHT, four[i].format);
	}
}

// Each target cleared by a resolve; the first draw over the left half with every target written, the second over the
// top half with each blended and masked its own way; each resolved again.
static bool draw_four(const Scene *scene, const XsdkTexture textures[XSDK_COLOR_TARGETS])
{
	XsdkColorTarget targets[XSDK_COLOR_TARGETS];
	place_four(targets);
	float first[XSDK_COLOR_TARGETS][CHANNELS];
	float second[XSDK_COLOR_TARGETS][CHANNELS];
	XsdkBlendState blends[XSDK_COLOR_TARGETS];
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		memcpy(first[i], four[i].first, sizeof first[i]);
		memcpy(second[i], four[i].second, sizeof second[i]);
		xsdk_gpu_blend_state(&blends[i], &four[i].blend);
	}
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, targets, XSDK_COLOR_TARGETS, depth_tile());
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &textures[i], four[i].clear);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, targets, XSDK_COLOR_TARGETS, depth_tile());
	xsdk_gpu_set_shaders(&scene->vertex, &scene->four_pixel);
	xsdk_gpu_set_vertices(0, scene->vertices, sizeof square);
	xsdk_gpu_set_pixel_constants(0, &first[0][0], XSDK_COLOR_TARGETS);
	draw_rectangle(0, 0, QUAD_RIGHT, TARGET_HEIGHT);
	xsdk_gpu_set_pixel_constants(0, &second[0][0], XSDK_COLOR_TARGETS);
	xsdk_gpu_set_blends(blends, XSDK_COLOR_TARGETS);
	draw_rectangle(0, 0, TARGET_WIDTH, BLEND_BOTTOM);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		xsdk_gpu_resolve_target(i, &textures[i], four[i].clear);
	xsdk_gpu_set_resolve_clears(true, true);
	return finished("four at once");
}

static uint32_t quarter_of(uint32_t x, uint32_t y)
{
	if (y < BLEND_BOTTOM)
		return x < QUAD_RIGHT ? QUARTER_BOTH : QUARTER_CLEAR_SECOND;
	return x < QUAD_RIGHT ? QUARTER_FIRST : QUARTER_CLEAR;
}

static uint32_t count_four_wrong(const FourTarget *target, const char *name)
{
	static const char *const quarters[QUARTERS] = { "both draws", "the clear and the second draw", "the first draw",
		"the clear" };
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < TARGET_HEIGHT; y++) {
		for (uint32_t x = 0; x < TARGET_WIDTH; x++) {
			const uint32_t quarter = quarter_of(x, y);
			if (texel_near(&target->holding, x, y, target->quarters[quarter]))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_texel(&target->holding, name, quarters[quarter], x, y, target->quarters[quarter]);
			wrong++;
		}
	}
	return wrong;
}

static void check_four(const Scene *scene)
{
	XsdkTexture textures[XSDK_COLOR_TARGETS] = { { .texels = NULL } };
	bool made = true;
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++)
		made = NT_SUCCESS(xsdk_texture_create(&textures[i], four[i].texture, TARGET_WIDTH, TARGET_HEIGHT, 1, true)) &&
			made;
	const bool drawn = made && draw_four(scene, textures);
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		char name[LABEL_BYTES];
		snprintf(name, sizeof name, "four at once: target %u, %s", (unsigned)i, four[i].what);
		if (drawn)
			xsdk_texture_download_rows(
				&textures[i], 0, 0, TARGET_HEIGHT, readback, TARGET_WIDTH * texel_bytes(&four[i].holding));
		const uint32_t wrong = drawn ? count_four_wrong(&four[i], name) : 0;
		char label[LABEL_BYTES + 16];
		snprintf(label, sizeof label, "targets: %s", name);
		check(label, drawn && wrong == 0);
	}
	for (uint32_t i = 0; i < XSDK_COLOR_TARGETS; i++) {
		if (textures[i].texels)
			xsdk_texture_free(&textures[i]);
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

static void make_blends(Scene *scene)
{
	const XsdkBlend opaque = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ZERO,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ZERO,
		.write_mask = XSDK_WRITE_ALL };
	const XsdkBlend additive = { .color_source = XSDK_BLEND_ONE,
		.color_dest = XSDK_BLEND_ONE,
		.alpha_source = XSDK_BLEND_ONE,
		.alpha_dest = XSDK_BLEND_ONE,
		.write_mask = XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&scene->opaque, &opaque);
	xsdk_gpu_blend_state(&scene->additive, &additive);
}

static bool load_shaders(Scene *scene)
{
	// microcode_pixel_shader writes c0 as the colour (tools/gen_microcode_shaders.py), four_targets_pixel_shader c0 to
	// c3 to the four targets (tools/gen_target_shaders.py).
	return NT_SUCCESS(xsdk_shader_load(&scene->vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&scene->pixel, microcode_pixel_shader, sizeof microcode_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&scene->four_pixel, four_targets_pixel_shader, sizeof four_targets_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&scene->vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
}

void test_targets(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("targets: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	Scene scene = {
		.vertex = { .microcode = NULL }, .pixel = { .microcode = NULL }, .four_pixel = { .microcode = NULL }
	};
	const bool loaded = load_shaders(&scene);
	volatile uint32_t *const vertices = upload_square();
	check("targets: shaders and vertices", loaded && vertices);
	make_blends(&scene);
	if (loaded && vertices) {
		scene.vertices = xsdk_gpu_address(vertices);
		for (uint32_t i = 0; i < CASES; i++)
			check_case(&scene, &cases[i]);
		check_four(&scene);
	}
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&scene.four_pixel);
	xsdk_shader_free(&scene.pixel);
	xsdk_shader_free(&scene.vertex);
	check("targets: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
