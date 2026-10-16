#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "fill_shaders.h"
#include "texture_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Times fills, sampled fills, small triangles and resolves, changing one piece of xsdk's draw state at a time.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	// The GPU's published clock, which the pixels a clock are counted against.
	GPU_MHZ = 500,
	BATCH_US = 100000,
	CALIBRATION_UNITS = 4,
	WARM_UP_UNITS = 2,
	MAX_UNITS = 4000,
	FENCE_TIMEOUT_US = 2000000,
	// Looks at the fence this far apart cost the GPU nothing (docs/HARDWARE.md, "Looking at a fence costs the GPU").
	FENCE_LOOK_US = 20,
	CLEAR_ARGB = 0xFF202020,
	CHANNEL_TOLERANCE = 3,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	TEXCOORD_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXTURE_SIZE = 256,
	TEXTURE_SLOT = 0,
	SAMPLES = 4,
	// 4x4 squares, two triangles each, over a quarter of the screen: 8 pixels a triangle.
	SQUARE_PIXELS = 4,
	GRID_COLUMNS = 160,
	GRID_ROWS = 90,
	GRID_TRIANGLES = GRID_COLUMNS * GRID_ROWS * 2,
	GRID_VERTICES = GRID_TRIANGLES * 3,
	GRID_PIXELS = GRID_COLUMNS * GRID_ROWS * SQUARE_PIXELS * SQUARE_PIXELS,
	// The same squares from shared corners, by 16-bit index.
	CORNER_COLUMNS = GRID_COLUMNS + 1,
	CORNERS = CORNER_COLUMNS * (GRID_ROWS + 1),
	// A draw's vertex count is 16 bits (xenia:src/xenia/gpu/registers.h:300-327): whole triangles, 8-byte index steps.
	DRAW_MAX_VERTICES = 65532,
	MAX_WRITES = 4,
};

// Registers by index (xenia:src/xenia/gpu/register_table.inc).
enum {
	WAIT_UNTIL = 0x05C8,
	PA_CL_ENHANCE = 0x0C85,
	SQ_GPR_MANAGEMENT = 0x0D00,
	SQ_FLOW_CONTROL = 0x0D01,
	RB_SURFACE_INFO = 0x2000,
	SQ_CONTEXT_MISC = 0x2181,
	SQ_INTERPOLATOR_CNTL = 0x2182,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_SU_SC_MODE_CNTL = 0x2205,
	RB_MODECONTROL = 0x2208,
	PA_SC_MPASS_PS_CNTL = 0x2292,
	PA_SC_VIZ_QUERY = 0x2293,
	VGT_ENHANCE = 0x2294,
	SQ_DEBUG_MISC_0 = 0x2309,
	SQ_DEBUG_MISC_1 = 0x230A,
	RB_SAMPLE_COUNT_CTL = 0x2324,
};

enum {
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	// A type-0 header's bit 15 sends every value to the same register (libxenon:drivers/xenos/xe.c:25-31).
	TYPE0_ONE_REGISTER = 1 << 15,
	// sc_sample_cntl in bits 2-3 (xenia:src/xenia/gpu/registers.h:154-157; xenos.h:804-808).
	CENTROIDS_ONLY = 0 << 2,
	CENTERS_ONLY = 1 << 2,
	CENTROIDS_AND_CENTERS = 2 << 2,
	// SQ_CONTEXT_MISC's optimize bits (xenia:src/xenia/gpu/registers.h:155, 236).
	INST_PRED_OPTIMIZE = 1 << 0,
	YIELD_OPTIMIZE = 1 << 17,
	// param_shade 0 (registers.h:247), all still at centres, where xsdk writes libxenon's 0xFFFFFFFF (xe.c:1400).
	INTERPOLATOR_CNTL_SHADE_0 = 0xFFFF0000,
	// What xsdk_gpu_set_target writes: the window offset on, no culling (libxenon xe.c:1410).
	MODE_CNTL_XSDK = 1 << 16,
	// PA_SU_SC_MODE_CNTL's scan converter bits (xenia:src/xenia/gpu/registers.h:479-483).
	PERSP_CORR_DIS = 1 << 20,
	QUAD_ORDER_ENABLE = 1 << 23,
	SC_ONE_QUAD_PER_CLOCK = 1 << 24,
	// xsdk_gpu_set_target's D3D clip space, and clip_disable (xenia:src/xenia/gpu/registers.h:547-550).
	CLIP_D3D_SPACE = 1 << 19,
	CLIP_DISABLE = 1 << 16,
	// Three corners, the fourth across the longest edge (xenia:src/xenia/gpu/xenos.h:53, d3d12/pipeline_cache.cc:2551).
	RECTANGLE_LIST = 8,
	RECTANGLE_VERTICES = 3,
	// The surface's pitch, with no HiZ pitch from bit 18 (xenia:src/xenia/gpu/registers.h:676).
	SURFACE_INFO_NO_HIZ = WIDTH,
	// EdramMode (xenia:src/xenia/gpu/xenos.h:886-911).
	EDRAM_DEPTH_ONLY = 5,
	// xsdk's, libxenon's, and the kernel's (docs/HARDWARE.md, "libxenon's set-up makes the resolves work").
	PA_CL_ENHANCE_XSDK = 3,
	PA_CL_ENHANCE_KERNEL = 7,
	SQ_FLOW_CONTROL_XSDK = 0x04000000,
	SQ_FLOW_CONTROL_KERNEL = 0x04007000,
	// Vertex shaders' share in bits 12-19, pixel shaders' in bits 4-11, out of 128 (src/gpu_setup.c).
	SPLIT_TOTAL = 0x80,
	SPLIT_XSDK_VERTEX = 0x40,
	SPLIT_REPEATS = 8,
	SPLIT_DYNAMIC = 1,
	// SQ_PROGRAM_CNTL's pixel shader register count less one (xenia:src/xenia/gpu/registers.h:136).
	PS_REGISTERS_SHIFT = 8,
	PS_REGISTERS_MASK = 0x3F << PS_REGISTERS_SHIFT,
};

typedef struct {
	uint32_t index;
	uint32_t value;
	// What the variant puts back after, for registers written once.
	uint32_t restore;
} RegisterWrite;

// The pixel shader a fill measurement draws with.
enum {
	FILL_CONSTANT,
	// oC0 = r0, the interpolated coordinates, for the opaque fill.
	FILL_INTERPOLANT,
	FILL_TWO_INSTRUCTIONS,
	FILL_UNSERIALIZED_ALLOC,
};

typedef struct {
	const char *name;
	// Written after each measurement's own state.
	RegisterWrite draw[MAX_WRITES];
	uint32_t draw_count;
	// Written once with the GPU idle, then put back.
	RegisterWrite once[MAX_WRITES];
	uint32_t once_count;
	// The vertex shaders' share of the register split, stepped to and back as libxenon steps it; 0 keeps xsdk's.
	uint32_t vertex_share;
	// The sampled shader's register count; 0 keeps its own 5.
	uint32_t sample_registers;
	uint32_t fill;
	// Colour from eDRAM tile 0 and depth after it, not depth then colour (xsdk_gpu_set_target_at).
	bool color_first;
	// Full-screen draws as one rectangle, not two triangles.
	bool rectangle;
} Variant;

// Each changes one piece of xsdk's state; xsdk asks for centres only since milestone 31.
static const Variant variants[] = {
	{ .name = "xsdk's state" },
	{ .name = "centroids and centres, as xost's shaders ask (SQ_CONTEXT_MISC sc_sample_cntl 2)",
		.draw = { { SQ_CONTEXT_MISC, CENTROIDS_AND_CENTERS } },
		.draw_count = 1 },
	{ .name = "centroids only (sc_sample_cntl 0)", .draw = { { SQ_CONTEXT_MISC, CENTROIDS_ONLY } }, .draw_count = 1 },
	{ .name = "SQ_CONTEXT_MISC inst_pred_optimize",
		.draw = { { SQ_CONTEXT_MISC, CENTERS_ONLY | INST_PRED_OPTIMIZE } },
		.draw_count = 1 },
	{ .name = "SQ_CONTEXT_MISC yield_optimize",
		.draw = { { SQ_CONTEXT_MISC, CENTERS_ONLY | YIELD_OPTIMIZE } },
		.draw_count = 1 },
	{ .name = "SQ_INTERPOLATOR_CNTL param_shade 0",
		.draw = { { SQ_INTERPOLATOR_CNTL, INTERPOLATOR_CNTL_SHADE_0 } },
		.draw_count = 1 },
	{ .name = "PA_SU_SC_MODE_CNTL sc_one_quad_per_clock",
		.draw = { { PA_SU_SC_MODE_CNTL, MODE_CNTL_XSDK | SC_ONE_QUAD_PER_CLOCK } },
		.draw_count = 1 },
	{ .name = "PA_SU_SC_MODE_CNTL quad_order_enable",
		.draw = { { PA_SU_SC_MODE_CNTL, MODE_CNTL_XSDK | QUAD_ORDER_ENABLE } },
		.draw_count = 1 },
	{ .name = "PA_SU_SC_MODE_CNTL persp_corr_dis",
		.draw = { { PA_SU_SC_MODE_CNTL, MODE_CNTL_XSDK | PERSP_CORR_DIS } },
		.draw_count = 1 },
	{ .name = "RB_SURFACE_INFO without the HiZ pitch",
		.draw = { { RB_SURFACE_INFO, SURFACE_INFO_NO_HIZ } },
		.draw_count = 1 },
	{ .name = "PA_SC_VIZ_QUERY 0", .draw = { { PA_SC_VIZ_QUERY, 0 } }, .draw_count = 1 },
	{ .name = "PA_SC_MPASS_PS_CNTL 0", .draw = { { PA_SC_MPASS_PS_CNTL, 0 } }, .draw_count = 1 },
	{ .name = "VGT_ENHANCE 0", .draw = { { VGT_ENHANCE, 0 } }, .draw_count = 1 },
	{ .name = "SQ_DEBUG_MISC_0 and _1 0", .draw = { { SQ_DEBUG_MISC_0, 0 }, { SQ_DEBUG_MISC_1, 0 } }, .draw_count = 2 },
	{ .name = "RB_SAMPLE_COUNT_CTL 0", .draw = { { RB_SAMPLE_COUNT_CTL, 0 } }, .draw_count = 1 },
	{ .name = "fill reading its interpolant", .fill = FILL_INTERPOLANT },
	{ .name = "fill through two instructions", .fill = FILL_TWO_INSTRUCTIONS },
	{ .name = "fill with its alloc unserialized", .fill = FILL_UNSERIALIZED_ALLOC },
	{ .name = "colour first in eDRAM", .color_first = true },
	{ .name = "sampled shader with 12 registers", .sample_registers = 12 },
	{ .name = "PA_CL_ENHANCE 7, the kernel's",
		.once = { { PA_CL_ENHANCE, PA_CL_ENHANCE_KERNEL, PA_CL_ENHANCE_XSDK } },
		.once_count = 1 },
	{ .name = "SQ_FLOW_CONTROL 0x04007000, the kernel's",
		.once = { { SQ_FLOW_CONTROL, SQ_FLOW_CONTROL_KERNEL, SQ_FLOW_CONTROL_XSDK } },
		.once_count = 1 },
	{ .name = "split 16 vertex, 112 pixel", .vertex_share = 0x10 },
	{ .name = "split 16 vertex, 112 pixel, sampled shader with 12 registers",
		.vertex_share = 0x10,
		.sample_registers = 12 },
	{ .name = "split 96 vertex, 32 pixel", .vertex_share = 0x60 },
	// A rectangle as the resolve draws one, with clipping off as it has it (libxenon xe.c:1140).
	{ .name = "full-screen draws as one rectangle",
		.draw = { { PA_CL_CLIP_CNTL, CLIP_D3D_SPACE | CLIP_DISABLE } },
		.draw_count = 1,
		.rectangle = true },
};

enum {
	VARIANTS = sizeof variants / sizeof variants[0],
};

enum {
	FILL_OPAQUE,
	FILL_BLENDED,
	FILL_DEPTH,
	FILL_DEPTH_ONLY_MODE,
	SAMPLED_POINT,
	SAMPLED_BILINEAR,
	TRIANGLES,
	TRIANGLES_INDEXED,
	RESOLVES,
	MEASURES,
};

static const char *const measure_names[MEASURES] = {
	"opaque fill",
	"blended fill",
	"depth fill, colour masked",
	"depth fill, kDepthOnly",
	"4 point samples",
	"4 bilinear samples",
	"8-pixel triangles",
	"8-pixel triangles, indexed",
	"resolves",
};

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

enum {
	SAMPLER_POINT,
	SAMPLER_BILINEAR,
	SAMPLERS,
};

enum {
	DEPTH_OFF,
	DEPTH_WRITE_ALWAYS,
	DEPTH_TEST_LESS,
	DEPTH_STATES,
};

enum {
	BLEND_OPAQUE,
	BLEND_ALPHA,
	BLEND_NO_COLOR,
	BLEND_STATES,
};

static const float fill_color[4] = { 0.25f, 0.5f, 0.75f, 0.5f };
static const float check_color[4] = { 0.875f, 0.0625f, 0.0625f, 1.0f };
static const float sample_scale[4] = { 0.25f, 0.25f, 0.25f, 0.25f };

static XsdkShader vertex_shader;
static XsdkShader fill_shader;
static XsdkShader interpolant_shader;
static XsdkShader unserialized_shader;
static XsdkShader two_shader;
static XsdkShader sample_shader;
static XsdkTexture texture;
static XsdkSamplerState samplers[SAMPLERS];
static XsdkDepthStencilState depth_states[DEPTH_STATES];
static XsdkBlendState blend_states[BLEND_STATES];
static volatile uint32_t *vertices;
static volatile uint32_t *indices;
static volatile uint32_t *pixels;
static uint32_t quad_address;
static uint32_t depth_quad_address;
static uint32_t check_quad_address;
static uint32_t grid_address;
static uint32_t corners_address;
static const Variant *current;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

// Spins on the time base between looks, as xsdk_gpu_wait does, never sleeping; false if the GPU never gets there.
static bool finish(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	uint64_t now = start;
	while (!xsdk_gpu_fence_done(fence)) {
		if (now - start > FENCE_TIMEOUT_US)
			return false;
		const uint64_t next = now + FENCE_LOOK_US;
		while ((now = now_us()) < next) {
		}
	}
	return true;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

// One step of Xe_pGInit1: the split written 8 times with bit 0, then once without (libxenon xe.c:443-452).
static void put_split(uint32_t vertex_share)
{
	const uint32_t split = vertex_share << 12 | (SPLIT_TOTAL - vertex_share) << 4;
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + SPLIT_REPEATS);
	packet[0] = (SPLIT_REPEATS - 1) << 16 | TYPE0_ONE_REGISTER | SQ_GPR_MANAGEMENT;
	for (uint32_t i = 0; i < SPLIT_REPEATS; i++)
		packet[1 + i] = split | SPLIT_DYNAMIC;
	put_register(SQ_GPR_MANAGEMENT, split);
}

static void step_split(uint32_t from, uint32_t to)
{
	while (from != to) {
		from = from < to ? from + 1 : from - 1;
		put_split(from);
	}
}

static void set_target(void)
{
	if (current->color_first)
		xsdk_gpu_set_target_at(WIDTH, HEIGHT, 0, xsdk_gpu_target_tiles(WIDTH, HEIGHT));
	else
		xsdk_gpu_set_target(WIDTH, HEIGHT);
}

static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

static void put_vertex(volatile uint32_t *at, float x, float y, float z, float u, float v)
{
	const float vertex[VERTEX_FLOATS] = { clip_x(x), clip_y(y), z, 1.0f, u, v };
	uint32_t words[VERTEX_FLOATS];
	memcpy(words, vertex, sizeof vertex);
	for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
		at[i] = words[i];
}

// Edges half a pixel before the pixels, for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837); a texel a pixel.
static void put_rectangle(volatile uint32_t *at, float left, float top, float right, float bottom, float z)
{
	const float u0 = left / TEXTURE_SIZE;
	const float v0 = top / TEXTURE_SIZE;
	const float u1 = right / TEXTURE_SIZE;
	const float v1 = bottom / TEXTURE_SIZE;
	const float x0 = left - 0.5f;
	const float y0 = top - 0.5f;
	const float x1 = right - 0.5f;
	const float y1 = bottom - 0.5f;
	put_vertex(at, x0, y0, z, u0, v0);
	put_vertex(at + VERTEX_FLOATS, x1, y0, z, u1, v0);
	put_vertex(at + 2 * VERTEX_FLOATS, x0, y1, z, u0, v1);
	put_vertex(at + 3 * VERTEX_FLOATS, x0, y1, z, u0, v1);
	put_vertex(at + 4 * VERTEX_FLOATS, x1, y0, z, u1, v0);
	put_vertex(at + 5 * VERTEX_FLOATS, x1, y1, z, u1, v1);
}

// Rises by 2 a texel to 255, then falls, so neighbours stay within 2 across the wrap too and filter to the texel.
static uint32_t ramp(uint32_t i)
{
	i %= TEXTURE_SIZE;
	return i < TEXTURE_SIZE / 2 ? 2 * i : 2 * (TEXTURE_SIZE - 1 - i) + 1;
}

static uint32_t texel(uint32_t x, uint32_t y)
{
	return 0xFF000080 | ramp(x) << 16 | ramp(y) << 8;
}

static bool load_shaders(void)
{
	return NT_SUCCESS(xsdk_shader_load(&vertex_shader, texture_vertex_shader, sizeof texture_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&fill_shader, fill_pixel_shader, sizeof fill_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&interpolant_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(
			&unserialized_shader, fill_unserialized_pixel_shader, sizeof fill_unserialized_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&two_shader, fill_two_pixel_shader, sizeof fill_two_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_load(&sample_shader, sample_pixel_shader, sizeof sample_pixel_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, vertex_layout, 2, (const uint32_t[]){ STRIDE_BYTES }));
}

static bool make_texture(void)
{
	if (!NT_SUCCESS(xsdk_texture_create(&texture, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true)))
		return false;
	static uint32_t texels[TEXTURE_SIZE * TEXTURE_SIZE];
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++)
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			texels[y * TEXTURE_SIZE + x] = texel(x, y);
	xsdk_texture_upload(&texture, 0, texels, TEXTURE_SIZE * 4);
	xsdk_gpu_invalidate(texture.address, texture.size_bytes);
	return true;
}

// The same squares as the grid, from their shared corners: two triangles a square, wound as put_rectangle winds them.
static void put_corners(volatile uint32_t *corners, volatile uint32_t *index_words)
{
	for (uint32_t row = 0; row <= GRID_ROWS; row++)
		for (uint32_t column = 0; column < CORNER_COLUMNS; column++)
			put_vertex(corners + (row * CORNER_COLUMNS + column) * VERTEX_FLOATS, column * SQUARE_PIXELS - 0.5f,
				row * SQUARE_PIXELS - 0.5f, 0.0f, 0.0f, 0.0f);
	uint32_t pending = 0;
	uint32_t count = 0;
	for (uint32_t row = 0; row < GRID_ROWS; row++) {
		for (uint32_t column = 0; column < GRID_COLUMNS; column++) {
			const uint32_t top_left = row * CORNER_COLUMNS + column;
			const uint32_t square[6] = { top_left, top_left + 1, top_left + CORNER_COLUMNS, top_left + CORNER_COLUMNS,
				top_left + 1, top_left + CORNER_COLUMNS + 1 };
			// Big-endian 16-bit indices, the first in each word's high half.
			for (uint32_t i = 0; i < 6; i++, count++) {
				pending = pending << 16 | square[i];
				if (count % 2)
					index_words[count / 2] = pending;
			}
		}
	}
}

static bool make_resources(void)
{
	if (!load_shaders() || !make_texture())
		return false;
	const uint32_t grid_offset = 3 * QUAD_BYTES;
	const uint32_t corners_offset = grid_offset + GRID_VERTICES * STRIDE_BYTES;
	const uint32_t vertex_bytes = corners_offset + CORNERS * STRIDE_BYTES;
	vertices = xsdk_gpu_alloc(vertex_bytes);
	indices = xsdk_gpu_alloc(GRID_VERTICES * 2);
	pixels = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertices || !indices || !pixels)
		return false;
	put_rectangle(vertices, 0, 0, WIDTH, HEIGHT, 0.0f);
	put_rectangle(vertices + QUAD_BYTES / 4, 0, 0, WIDTH, HEIGHT, 0.25f);
	put_rectangle(vertices + 2 * QUAD_BYTES / 4, 0, 0, WIDTH, HEIGHT, 0.5f);
	volatile uint32_t *grid = vertices + grid_offset / 4;
	for (uint32_t row = 0; row < GRID_ROWS; row++) {
		for (uint32_t column = 0; column < GRID_COLUMNS; column++) {
			const float left = (float)(column * SQUARE_PIXELS);
			const float top = (float)(row * SQUARE_PIXELS);
			put_rectangle(grid, left, top, left + SQUARE_PIXELS, top + SQUARE_PIXELS, 0.0f);
			grid += QUAD_BYTES / 4;
		}
	}
	put_corners(vertices + corners_offset / 4, indices);
	__asm__ volatile("sync" ::: "memory");
	const uint32_t base = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(base, vertex_bytes);
	xsdk_gpu_invalidate(xsdk_gpu_address(indices), GRID_VERTICES * 2);
	quad_address = base;
	depth_quad_address = base + QUAD_BYTES;
	check_quad_address = base + 2 * QUAD_BYTES;
	grid_address = base + grid_offset;
	corners_address = base + corners_offset;
	return true;
}

static void make_states(void)
{
	const XsdkSampler point = { XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_ADDRESS_WRAP,
		XSDK_ADDRESS_WRAP, 0.0f, 0, 0, 0 };
	const XsdkSampler bilinear = { XSDK_FILTER_LINEAR, XSDK_FILTER_LINEAR, XSDK_FILTER_POINT, XSDK_ADDRESS_WRAP,
		XSDK_ADDRESS_WRAP, 0.0f, 0, 0, 0 };
	xsdk_texture_sampler_state(&samplers[SAMPLER_POINT], &point);
	xsdk_texture_sampler_state(&samplers[SAMPLER_BILINEAR], &bilinear);
	const XsdkDepthStencil off = { .depth_compare = XSDK_COMPARE_ALWAYS, .stencil_compare = XSDK_COMPARE_ALWAYS };
	XsdkDepthStencil write = off;
	write.depth_test = true;
	write.depth_write = true;
	XsdkDepthStencil less = off;
	less.depth_test = true;
	less.depth_compare = XSDK_COMPARE_LESS;
	xsdk_gpu_depth_stencil_state(&depth_states[DEPTH_OFF], &off);
	xsdk_gpu_depth_stencil_state(&depth_states[DEPTH_WRITE_ALWAYS], &write);
	xsdk_gpu_depth_stencil_state(&depth_states[DEPTH_TEST_LESS], &less);
	const XsdkBlend opaque = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
		XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	const XsdkBlend alpha = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	XsdkBlend masked = opaque;
	masked.write_mask = 0;
	xsdk_gpu_blend_state(&blend_states[BLEND_OPAQUE], &opaque);
	xsdk_gpu_blend_state(&blend_states[BLEND_ALPHA], &alpha);
	xsdk_gpu_blend_state(&blend_states[BLEND_NO_COLOR], &masked);
}

static void put_draw_writes(void)
{
	for (uint32_t i = 0; i < current->draw_count; i++)
		put_register(current->draw[i].index, current->draw[i].value);
}

static const XsdkShader *pixel_shader_for(uint32_t measure, const XsdkShader *sampler_shader)
{
	if (measure == SAMPLED_POINT || measure == SAMPLED_BILINEAR)
		return sampler_shader;
	if (current->fill == FILL_INTERPOLANT)
		return measure == FILL_OPAQUE ? &interpolant_shader : &fill_shader;
	if (current->fill == FILL_TWO_INSTRUCTIONS)
		return &two_shader;
	return current->fill == FILL_UNSERIALIZED_ALLOC ? &unserialized_shader : &fill_shader;
}

// The measurement's draw state, then the variant's registers over it.
static void set_state(uint32_t measure, const XsdkShader *sampler_shader)
{
	set_target();
	if (measure == RESOLVES) {
		put_draw_writes();
		return;
	}
	const bool sampled = measure == SAMPLED_POINT || measure == SAMPLED_BILINEAR;
	xsdk_gpu_set_shaders(&vertex_shader, pixel_shader_for(measure, sampler_shader));
	xsdk_gpu_set_pixel_constants(0, sampled ? sample_scale : fill_color, 1);
	const bool depth = measure == FILL_DEPTH || measure == FILL_DEPTH_ONLY_MODE;
	xsdk_gpu_set_vertices(0, depth ? depth_quad_address : quad_address, QUAD_BYTES);
	if (sampled)
		xsdk_gpu_set_texture(
			TEXTURE_SLOT, &texture, &samplers[measure == SAMPLED_BILINEAR ? SAMPLER_BILINEAR : SAMPLER_POINT]);
	xsdk_gpu_set_depth_stencil(&depth_states[depth ? DEPTH_WRITE_ALWAYS : DEPTH_OFF]);
	xsdk_gpu_set_blend(&blend_states[depth ? BLEND_NO_COLOR : measure == FILL_BLENDED ? BLEND_ALPHA : BLEND_OPAQUE]);
	if (measure == FILL_DEPTH_ONLY_MODE)
		put_register(RB_MODECONTROL, EDRAM_DEPTH_ONLY);
	if (measure == TRIANGLES_INDEXED)
		xsdk_gpu_set_vertices(0, corners_address, CORNERS * STRIDE_BYTES);
	put_draw_writes();
}

static uint32_t draw_vertices(uint32_t first)
{
	return GRID_VERTICES - first < DRAW_MAX_VERTICES ? GRID_VERTICES - first : DRAW_MAX_VERTICES;
}

static void put_unit(uint32_t measure)
{
	if (measure == RESOLVES) {
		xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
		return;
	}
	if (measure == TRIANGLES) {
		for (uint32_t first = 0; first < GRID_VERTICES; first += DRAW_MAX_VERTICES) {
			xsdk_gpu_set_vertices(0, grid_address + first * STRIDE_BYTES, draw_vertices(first) * STRIDE_BYTES);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, draw_vertices(first));
		}
		return;
	}
	if (measure == TRIANGLES_INDEXED) {
		for (uint32_t first = 0; first < GRID_VERTICES; first += DRAW_MAX_VERTICES)
			xsdk_gpu_draw_indexed(
				XSDK_PRIMITIVE_TRIANGLE_LIST, xsdk_gpu_address(indices) + first * 2, draw_vertices(first), false);
		return;
	}
	if (current->rectangle)
		xsdk_gpu_draw(RECTANGLE_LIST, RECTANGLE_VERTICES);
	else
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// Microseconds the GPU took for count units after the state; 0 if it hung.
static uint64_t time_units(uint32_t measure, uint32_t count)
{
	const uint64_t start = now_us();
	for (uint32_t i = 0; i < count; i++)
		put_unit(measure);
	if (!finish())
		return 0;
	const uint64_t elapsed = now_us() - start;
	return elapsed ? elapsed : 1;
}

static uint32_t stored_argb(uint32_t x, uint32_t y)
{
	return __builtin_bswap32(pixels[tiled_index(x, y, WIDTH)]);
}

static bool near(uint32_t a, uint32_t b)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t channel_a = a >> shift & 0xFF;
		const uint32_t channel_b = b >> shift & 0xFF;
		if ((channel_a > channel_b ? channel_a - channel_b : channel_b - channel_a) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static uint32_t packed(const float color[4])
{
	const uint32_t a = (uint32_t)(color[3] * 255.0f + 0.5f);
	const uint32_t r = (uint32_t)(color[0] * 255.0f + 0.5f);
	const uint32_t g = (uint32_t)(color[1] * 255.0f + 0.5f);
	const uint32_t b = (uint32_t)(color[2] * 255.0f + 0.5f);
	return a << 24 | r << 16 | g << 8 | b;
}

// A depth fill drew if a quad behind it then draws nowhere.
static void put_depth_check(void)
{
	set_target();
	xsdk_gpu_set_shaders(&vertex_shader, &fill_shader);
	xsdk_gpu_set_pixel_constants(0, check_color, 1);
	xsdk_gpu_set_vertices(0, check_quad_address, QUAD_BYTES);
	xsdk_gpu_set_depth_stencil(&depth_states[DEPTH_TEST_LESS]);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static const uint32_t check_points[][2] = { { 0, 0 }, { 640, 360 }, { 1279, 719 }, { 300, 500 }, { 637, 357 } };

enum {
	CHECK_POINTS = sizeof check_points / sizeof check_points[0],
};

// What an interpolant fill writes: the texture coordinates, red and green from 0 at the left and top edges.
static uint32_t coordinate_argb(uint32_t x, uint32_t y)
{
	const float color[4] = { (x + 0.5f) / TEXTURE_SIZE, (y + 0.5f) / TEXTURE_SIZE, 0.0f, 1.0f };
	const float clamped[4] = { color[0] > 1.0f ? 1.0f : color[0], color[1] > 1.0f ? 1.0f : color[1], 0.0f, 1.0f };
	return packed(clamped);
}

static uint32_t expected_argb(uint32_t measure, uint32_t x, uint32_t y)
{
	switch (measure) {
	case FILL_OPAQUE:
	case FILL_BLENDED:
		return current->fill == FILL_INTERPOLANT && measure == FILL_OPAQUE ? coordinate_argb(x, y) : packed(fill_color);
	case SAMPLED_POINT:
	case SAMPLED_BILINEAR:
		return texel(x, y);
	case TRIANGLES:
	case TRIANGLES_INDEXED:
		return x < GRID_COLUMNS * SQUARE_PIXELS && y < GRID_ROWS * SQUARE_PIXELS ? packed(fill_color) : CLEAR_ARGB;
	default:
		return CLEAR_ARGB;
	}
}

// Resolves what the measurement left and checks it against what it should have drawn; the resolve clears it too.
static bool check(uint32_t measure, uint32_t *wrong_argb, uint32_t *wrong_point)
{
	if (measure == FILL_DEPTH || measure == FILL_DEPTH_ONLY_MODE)
		put_depth_check();
	if (measure != RESOLVES)
		xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!finish())
		return false;
	for (uint32_t i = 0; i < CHECK_POINTS; i++) {
		const uint32_t x = check_points[i][0];
		const uint32_t y = check_points[i][1];
		const uint32_t stored = stored_argb(x, y);
		if (!near(stored, expected_argb(measure, x, y))) {
			*wrong_argb = stored;
			*wrong_point = i;
			return false;
		}
	}
	return true;
}

static uint64_t unit_pixels(uint32_t measure)
{
	return measure == TRIANGLES || measure == TRIANGLES_INDEXED ? GRID_PIXELS : PIXELS;
}

static void log_measure(
	uint32_t measure, uint32_t count, uint64_t elapsed_us, bool drew, uint32_t wrong_argb, uint32_t wrong_point)
{
	const uint64_t mpixels = unit_pixels(measure) * count / elapsed_us;
	char extra[64] = "";
	if (measure == TRIANGLES || measure == TRIANGLES_INDEXED)
		(void)snprintf(
			extra, sizeof extra, ", %u Mtriangles/s", (unsigned)((uint64_t)GRID_TRIANGLES * count / elapsed_us));
	if (measure == SAMPLED_POINT || measure == SAMPLED_BILINEAR)
		(void)snprintf(extra, sizeof extra, ", %u Mtexels/s", (unsigned)(mpixels * SAMPLES));
	char outcome[64] = "drew right";
	if (!drew)
		(void)snprintf(outcome, sizeof outcome, "WRONG: %08X at (%u, %u)", (unsigned)wrong_argb,
			(unsigned)check_points[wrong_point][0], (unsigned)check_points[wrong_point][1]);
	xsdk_log_write("pixel rate: %s: %s: %u in %u us: %u Mpixels/s, %u.%02u a clock%s; %s", current->name,
		measure_names[measure], (unsigned)count, (unsigned)elapsed_us, (unsigned)mpixels, (unsigned)(mpixels / GPU_MHZ),
		(unsigned)(mpixels * 100 / GPU_MHZ % 100), extra, outcome);
}

// One measurement under the variant: a warm-up, a short batch to size the timed one, then about 100 ms of units.
static bool run_measure(uint32_t measure, const XsdkShader *sampler_shader)
{
	set_state(measure, sampler_shader);
	const uint64_t warm_us = time_units(measure, WARM_UP_UNITS);
	const uint64_t calibration_us = warm_us ? time_units(measure, CALIBRATION_UNITS) : 0;
	uint64_t count = calibration_us ? BATCH_US * CALIBRATION_UNITS / calibration_us : 0;
	count = count < CALIBRATION_UNITS ? CALIBRATION_UNITS : count > MAX_UNITS ? MAX_UNITS : count;
	const uint64_t elapsed_us = calibration_us ? time_units(measure, (uint32_t)count) : 0;
	if (!elapsed_us) {
		xsdk_log_write("pixel rate: %s: %s: the GPU didn't finish; stopping", current->name, measure_names[measure]);
		return false;
	}
	uint32_t wrong_argb = 0;
	uint32_t wrong_point = 0;
	const bool drew = check(measure, &wrong_argb, &wrong_point);
	log_measure(measure, (uint32_t)count, elapsed_us, drew, wrong_argb, wrong_point);
	return true;
}

static void put_once(bool restore)
{
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	for (uint32_t i = 0; i < current->once_count; i++)
		put_register(current->once[i].index, restore ? current->once[i].restore : current->once[i].value);
	if (current->vertex_share)
		step_split(
			restore ? current->vertex_share : SPLIT_XSDK_VERTEX, restore ? SPLIT_XSDK_VERTEX : current->vertex_share);
}

static bool run_variant(const Variant *variant)
{
	current = variant;
	XsdkShader sampler_shader = sample_shader;
	if (variant->sample_registers)
		sampler_shader.program_control = (sample_shader.program_control & ~(uint32_t)PS_REGISTERS_MASK) |
			(variant->sample_registers - 1) << PS_REGISTERS_SHIFT;
	put_once(false);
	bool ran = true;
	for (uint32_t measure = 0; measure < MEASURES && ran; measure++)
		ran = run_measure(measure, &sampler_shader);
	put_once(true);
	return ran && finish();
}

// Drawing registers read as XAM left them (docs/HARDWARE.md, "libxenon's set-up makes the resolves work").
static const uint32_t logged_registers[] = { 0x0C85, 0x0D00, 0x0D01, 0x0D02, 0x0D04, 0x0E42, 0x0F01, 0x0F02, 0x2000,
	0x2001, 0x2002, 0x2104, 0x2180, 0x2181, 0x2182, 0x2200, 0x2201, 0x2202, 0x2203, 0x2204, 0x2205, 0x2206, 0x2208,
	0x2284, 0x2292, 0x2293, 0x2294, 0x2300, 0x2301, 0x2302, 0x2309, 0x230A, 0x2312, 0x2316, 0x2317, 0x2324 };

enum {
	LOGGED_REGISTERS = sizeof logged_registers / sizeof logged_registers[0],
	REGISTERS_A_LINE = 9,
};

static void log_registers(const char *when)
{
	volatile const uint32_t *const registers = (volatile const uint32_t *)0x7FC80000;
	for (uint32_t i = 0; i < LOGGED_REGISTERS; i += REGISTERS_A_LINE) {
		char line[160];
		uint32_t length = 0;
		for (uint32_t j = i; j < LOGGED_REGISTERS && j < i + REGISTERS_A_LINE; j++)
			length += (uint32_t)snprintf(line + length, sizeof line - length, " %04X %08X",
				(unsigned)logged_registers[j], (unsigned)registers[logged_registers[j]]);
		xsdk_log_write("pixel rate: %s:%s", when, line);
	}
}

int main(void)
{
	xsdk_log_write("pixel rate probe");
	log_registers("as the console left them");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("pixel rate: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!make_resources()) {
		xsdk_log_write("pixel rate: setup failed");
		return 1;
	}
	make_states();
	xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
	bool ran = finish();
	for (uint32_t i = 0; i < VARIANTS && ran; i++)
		ran = run_variant(&variants[i]);
	xsdk_log_write("pixel rate: %s", ran ? "done" : "stopped");
	xsdk_log_write("pixel rate: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
