#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "constant_shaders.h"
#include "microcode_shaders.h"
#include "tests.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Render target formats (vulkan4xenon milestone 25): a target of each format, cleared by a resolve, a quad drawn over
// its left half, then resolved into a texture of the format it converts to, every texel read back and checked by value.
// The top of its right half takes the quad twice, the second time blended onto the first, which doubles it where the
// format blends.

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
	LABEL_BYTES = 96,
};

// How a texture holds a texel.
enum {
	// One word: red, green and blue in 10 bits from the low end, alpha in the top 2.
	KIND_10_10_10_2,
	// One half float a channel, red first.
	KIND_HALVES,
	// One float, red.
	KIND_FLOATS,
};

// The texture's channels, the clear and the quad's colour, each channel apart from the others, past 1 where the format
// holds it. Doubled, the colour stays within the format, so blending shows.
typedef struct {
	const char *name;
	uint32_t target;
	uint32_t texture;
	uint32_t kind;
	uint32_t channels;
	float clear[CHANNELS];
	float color[CHANNELS];
	// How far a channel may be from what's drawn: a unorm's step, or a fixed point's and a half's.
	float tolerance;
	// vulkan4xenon reports blending for it, so its blend is a check; the others' is logged.
	bool blend_checked;
} Case;

static const Case cases[] = {
	{ "2_10_10_10", XSDK_TARGET_2_10_10_10, XSDK_FORMAT_A2B10G10R10, KIND_10_10_10_2, 4, { 0.2f, 0.4f, 0.6f, 1.0f },
		{ 0.45f, 0.35f, 0.05f, 1.0f / 3 }, 1.5f / 1023, true },
	{ "2_10_10_10_FLOAT", XSDK_TARGET_2_10_10_10_FLOAT, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 4,
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 3.0f, 1.0f / 3 }, 0.002f, false },
	{ "16_16_16_16", XSDK_TARGET_16_16_16_16, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 4, { 0.25f, 0.5f, 0.75f, 1.0f },
		{ 1.5f, 2.25f, 3.0f, -0.5f }, 0.002f, false },
	{ "16_16_FLOAT", XSDK_TARGET_16_16_FLOAT, XSDK_FORMAT_R16G16F, KIND_HALVES, 2, { 0.25f, 0.5f }, { 1.5f, 1000.0f },
		0.0f, false },
	{ "16_16_16_16_FLOAT", XSDK_TARGET_16_16_16_16_FLOAT, XSDK_FORMAT_R16G16B16A16F, KIND_HALVES, 4,
		{ 0.25f, 0.5f, 0.75f, 1.0f }, { 1.5f, 2.25f, 1000.0f, -2.0f }, 0.0f, false },
	{ "32_FLOAT", XSDK_TARGET_32_FLOAT, XSDK_FORMAT_R32F, KIND_FLOATS, 1, { 0.25f }, { 1234.5f }, 0.0f, false },
};

enum { CASES = sizeof cases / sizeof cases[0] };

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

// Channel c of texel (x, y), as the texture's format holds it.
static float texel_channel(const Case *test, uint32_t x, uint32_t y, uint32_t c)
{
	const uint32_t index = y * TARGET_WIDTH + x;
	if (test->kind == KIND_10_10_10_2) {
		const uint32_t word = readback[index];
		return c == 3 ? (float)(word >> 30) / 3.0f : (float)(word >> (10 * c) & 0x3FF) / 1023.0f;
	}
	if (test->kind == KIND_HALVES) {
		uint16_t half;
		memcpy(&half, (const uint8_t *)readback + (index * test->channels + c) * sizeof half, sizeof half);
		return half_value(half);
	}
	float value;
	memcpy(&value, &readback[index * test->channels + c], sizeof value);
	return value;
}

static bool texel_near(const Case *test, uint32_t x, uint32_t y, const float *want)
{
	for (uint32_t c = 0; c < test->channels; c++) {
		if (fabsf(texel_channel(test, x, y, c) - want[c]) > test->tolerance)
			return false;
	}
	return true;
}

static void log_texel(const Case *test, const char *what, uint32_t x, uint32_t y, const float *want)
{
	float got[CHANNELS] = { 0.0f };
	for (uint32_t c = 0; c < test->channels; c++)
		got[c] = texel_channel(test, x, y, c);
	xsdk_log_write("targets: %s: %s at (%u, %u) is %g %g %g %g, not %g %g %g %g", test->name, what, (unsigned)x,
		(unsigned)y, got[0], got[1], got[2], got[3], want[0], test->channels > 1 ? want[1] : 0.0f,
		test->channels > 2 ? want[2] : 0.0f, test->channels > 3 ? want[3] : 0.0f);
}

static uint32_t texel_bytes(const Case *test)
{
	return test->kind == KIND_10_10_10_2 ? 4 : (test->kind == KIND_HALVES ? 2 : 4) * test->channels;
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

// Clears with a resolve, which copies what eDRAM held; draws the quad and the blended one; resolves again.
static bool draw_case(const Scene *scene, const Case *test, XsdkTexture *texture)
{
	const XsdkColorTarget target = { .format = test->target, .tile = 0 };
	const uint32_t depth_tile = XSDK_EDRAM_TILES - xsdk_gpu_target_tiles(TARGET_WIDTH, TARGET_HEIGHT);
	float color[CHANNELS] = { 0.0f, 0.0f, 0.0f, 1.0f };
	memcpy(color, test->color, test->channels * sizeof color[0]);
	xsdk_gpu_set_resolve_clears(true, false);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, &target, 1, depth_tile);
	xsdk_gpu_resolve_target(0, texture, test->clear);
	xsdk_gpu_set_targets(TARGET_WIDTH, TARGET_HEIGHT, &target, 1, depth_tile);
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
	const bool landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	if (!landed)
		xsdk_log_write("targets: %s: the resolve never finished", test->name);
	return landed;
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
			if (texel_near(test, x, y, want))
				continue;
			if (wrong < MAX_MISSES_LOGGED)
				log_texel(test, quad ? "the quad" : "the clear", x, y, want);
			wrong++;
		}
	}
	return wrong;
}

// The blended rows: the colour doubled where the format blends.
static uint32_t count_unblended(const Case *test, bool logged)
{
	float doubled[CHANNELS];
	for (uint32_t c = 0; c < test->channels; c++)
		doubled[c] = 2.0f * test->color[c];
	uint32_t wrong = 0;
	for (uint32_t y = 0; y < BLEND_BOTTOM; y++) {
		for (uint32_t x = QUAD_RIGHT; x < TARGET_WIDTH; x++) {
			if (texel_near(test, x, y, doubled))
				continue;
			if (logged && wrong < MAX_MISSES_LOGGED)
				log_texel(test, "the quad blended onto itself", x, y, doubled);
			wrong++;
		}
	}
	return wrong;
}

static void check_case(const Scene *scene, const Case *test)
{
	XsdkTexture texture = { .texels = NULL };
	char name[LABEL_BYTES];
	const bool made = NT_SUCCESS(xsdk_texture_create(&texture, test->texture, TARGET_WIDTH, TARGET_HEIGHT, 1, true));
	const bool drawn = made && draw_case(scene, test, &texture);
	if (drawn)
		xsdk_texture_download_rows(&texture, 0, 0, TARGET_HEIGHT, readback, TARGET_WIDTH * texel_bytes(test));
	const uint32_t wrong = drawn ? count_wrong(test) : 0;
	snprintf(name, sizeof name, "targets: %s holds its clear and a quad's colour, each channel in place", test->name);
	check(name, drawn && wrong == 0);
	const uint32_t unblended = drawn ? count_unblended(test, test->blend_checked) : 0;
	if (test->blend_checked) {
		snprintf(name, sizeof name, "targets: %s blends a quad onto itself, doubling it", test->name);
		check(name, drawn && unblended == 0);
	} else if (drawn) {
		const float got[CHANNELS] = { texel_channel(test, QUAD_RIGHT, 0, 0),
			test->channels > 1 ? texel_channel(test, QUAD_RIGHT, 0, 1) : 0.0f,
			test->channels > 2 ? texel_channel(test, QUAD_RIGHT, 0, 2) : 0.0f,
			test->channels > 3 ? texel_channel(test, QUAD_RIGHT, 0, 3) : 0.0f };
		xsdk_log_write("targets: %s: a quad blended onto itself gave %g %g %g %g; %u of %u texels doubled", test->name,
			got[0], got[1], got[2], got[3], (unsigned)((TARGET_WIDTH - QUAD_RIGHT) * BLEND_BOTTOM - unblended),
			(unsigned)((TARGET_WIDTH - QUAD_RIGHT) * BLEND_BOTTOM));
	}
	if (made)
		xsdk_texture_free(&texture);
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

void test_targets(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("targets: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	Scene scene = { .vertex = { .microcode = NULL }, .pixel = { .microcode = NULL } };
	// microcode_pixel_shader writes c0 as the colour (tools/gen_microcode_shaders.py).
	const bool loaded =
		NT_SUCCESS(xsdk_shader_load(&scene.vertex, constant_vertex_shader, sizeof constant_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&scene.pixel, microcode_pixel_shader, sizeof microcode_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&scene.vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }));
	volatile uint32_t *const vertices = upload_square();
	check("targets: shaders and vertices", loaded && vertices);
	make_blends(&scene);
	if (loaded && vertices) {
		scene.vertices = xsdk_gpu_address(vertices);
		for (uint32_t i = 0; i < CASES; i++)
			check_case(&scene, &cases[i]);
	}
	xsdk_gpu_free(vertices);
	xsdk_shader_free(&scene.pixel);
	xsdk_shader_free(&scene.vertex);
	check("targets: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
