#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "draw_cost_shaders.h"
#include "tiled_index.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Times what vkbench's cube costs the GPU, a piece of the draw at a time (SCOPE.md, milestone 39). Each variant's draws
// are recorded once, a small batch and a large one; then the GPU is timed from the kick to the fence over each, so
// what the two share cancels and the difference is the draws'. The cubes are a few pixels, spread over the screen.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	ROUNDS = 5,
	SMALL = 500,
	LARGE = 2500,
	SIZES = 2,
	// A cell a cube, enough for the large batch.
	COLUMNS = 64,
	ROWS = 40,
	CUBE_PIXELS = 4,
	CUBE_VERTICES = 24,
	CUBE_INDICES = 36,
	VERTEX_FLOATS = 5,
	VERTEX_BYTES = VERTEX_FLOATS * 4,
	TEXTURE_SIZE = 64,
	TEXTURE_CHECKER = 8,
	TEXTURES = 2,
	MVP_VECTORS = 4,
	// vkbench's pixel shaders take the tint 64 bytes into the push constants, 4 registers on.
	TINT_OFFSET_REGISTERS = 4,
	// Constants loaded from memory come a group of 4, 64 bytes, at a time (docs/HARDWARE.md, "Constants loaded from
	// memory go in groups of 4 too").
	GROUP_VECTORS = 4,
	GROUP_FLOATS = GROUP_VECTORS * 4,
	GROUP_BYTES = GROUP_FLOATS * 4,
	// The most any variant writes a draw, with room to spare.
	MAX_DRAW_DWORDS = 128,
	// A texture's fetch constants: a group of 6 dwords at a slot (src/gpu_draw.c, src/gpu_private.h).
	SHADER_CONSTANT_FETCH_00_0 = 0x4800,
	FETCH_GROUP_DWORDS = 6,
	CLEAR_ARGB = 0xFF202020,
	FENCE_TIMEOUT_MS = 1000,
	// The time base ticks at 49.875 MHz (src/time.c).
	TICKS_PER_MS = 49875,
	// DRAW_INDX and its initiator's fields (xenia:src/xenia/gpu/xenos.h, PM4 opcodes; registers.h:300-327), as
	// xsdk_gpu_draw_indexed writes them (src/gpu_draw.c).
	PM4_DRAW_INDX = 0x22,
	VERTEX_COUNT_SHIFT = 16,
	DMA_SWAP_8IN16 = 1 << 30,
	// IM_LOAD and the program registers, as xsdk_gpu_set_shaders writes them (src/gpu_draw.c, src/gpu_private.h).
	PM4_IM_LOAD = 0x27,
	SQ_PROGRAM_CNTL = 0x2180,
	PROGRAM_REGISTERS = 3,
	SAMPLE_CNTL_MASK = 3 << 2,
	SAMPLE_CNTL_CENTERS_ONLY = 1 << 2,
	// Cells whose centres the check reads, and how many must show a cube.
	CHECKED_CELLS = 100,
	CHECKED_DRAWN = 90,
};

static const uint32_t INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF;

typedef struct {
	const char *name;
	// Records count draws of this variant.
	void (*record)(uint32_t count);
} Variant;

static XsdkShader vertex_shader;
static XsdkShader pixel_shaders[2];
// The matrix alone and the tint alone, no texture (tools/gen_draw_cost_shaders.py).
static XsdkShader plain_vertex_shader;
static XsdkShader flat_pixel_shader;
static XsdkTexture textures[TEXTURES];
static XsdkSamplerState sampler;
static XsdkBlendState opaque;
static XsdkBlendState alpha;
static XsdkDepthStencilState depth_test;
static XsdkDepthStencilState no_depth;
static XsdkBlendState no_colour;
static XsdkRasterState raster;
static volatile uint32_t *vertices;
static volatile uint32_t *indices;
// The cube's 36 corners in index order, for draws without indices; and its indices twice over.
static volatile uint32_t *unrolled;
static volatile uint32_t *doubled;
static uint32_t doubled_address;
// One triangle with no area: its three indices the same.
static volatile uint32_t *degenerate;
static uint32_t degenerate_address;
static volatile uint32_t *loaded;
static volatile uint32_t *offscreen;
static uint32_t index_address;
static float mvps[LARGE][MVP_VECTORS * 4];
static float tints[LARGE][4];

static uint64_t time_base(void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t check;
	// The upper half can tick between the two reads in 32-bit mode (src/time.c).
	do {
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(check));
	} while (upper != check);
	return (uint64_t)upper << 32 | lower;
}

static volatile uint32_t *gpu_copy(const void *data, uint32_t size_bytes)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(size_bytes);
	if (!memory)
		return NULL;
	const uint32_t *const words = data;
	for (uint32_t i = 0; i < size_bytes / 4; i++)
		memory[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
	return memory;
}

// vkbench's cube (vulkan4xenon:bench/assets.c): a unit cube, each face's corners with its texture coordinates.
static bool make_cube(void)
{
	static const float faces[6][3][3] = {
		{ { 1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 } },
		{ { -1, 0, 0 }, { 0, 0, 1 }, { 0, 1, 0 } },
		{ { 0, 1, 0 }, { 1, 0, 0 }, { 0, 0, -1 } },
		{ { 0, -1, 0 }, { 1, 0, 0 }, { 0, 0, 1 } },
		{ { 0, 0, 1 }, { 1, 0, 0 }, { 0, 1, 0 } },
		{ { 0, 0, -1 }, { -1, 0, 0 }, { 0, 1, 0 } },
	};
	float cube[CUBE_VERTICES][VERTEX_FLOATS];
	uint16_t cube_indices[CUBE_INDICES];
	for (int f = 0; f < 6; f++) {
		for (int c = 0; c < 4; c++) {
			const float su = (c & 1) ? 1.0f : -1.0f;
			const float sv = (c & 2) ? 1.0f : -1.0f;
			float *const v = cube[f * 4 + c];
			for (int axis = 0; axis < 3; axis++)
				v[axis] = 0.5f * (faces[f][0][axis] + su * faces[f][1][axis] + sv * faces[f][2][axis]);
			v[3] = (c & 1) ? 1.0f : 0.0f;
			v[4] = (c & 2) ? 0.0f : 1.0f;
		}
		const uint16_t base = (uint16_t)(f * 4);
		const uint16_t quad[] = { base, base + 1, base + 2, base + 2, base + 1, base + 3 };
		memcpy(&cube_indices[f * 6], quad, sizeof quad);
	}
	float corners[CUBE_INDICES][VERTEX_FLOATS];
	uint16_t twice[CUBE_INDICES * 2];
	for (uint32_t i = 0; i < CUBE_INDICES; i++) {
		memcpy(corners[i], cube[cube_indices[i]], sizeof corners[i]);
		twice[i] = twice[CUBE_INDICES + i] = cube_indices[i];
	}
	vertices = gpu_copy(cube, sizeof cube);
	indices = gpu_copy(cube_indices, sizeof cube_indices);
	const uint16_t same[] = { 0, 0, 0, 0 };
	unrolled = gpu_copy(corners, sizeof corners);
	doubled = gpu_copy(twice, sizeof twice);
	degenerate = gpu_copy(same, sizeof same);
	if (!vertices || !indices || !unrolled || !doubled || !degenerate)
		return false;
	index_address = xsdk_gpu_address(indices);
	doubled_address = xsdk_gpu_address(doubled);
	degenerate_address = xsdk_gpu_address(degenerate);
	return true;
}

// Each cube a few pixels across, at the centre of its cell, and its tint.
static void place_cubes(void)
{
	for (uint32_t c = 0; c < LARGE; c++) {
		float *const m = mvps[c];
		memset(m, 0, sizeof mvps[c]);
		m[0] = 2.0f * CUBE_PIXELS / WIDTH;
		m[5] = 2.0f * CUBE_PIXELS / HEIGHT;
		m[10] = 0.25f;
		m[12] = -1.0f + (2.0f * (float)(c % COLUMNS) + 1.0f) / COLUMNS;
		m[13] = -1.0f + (2.0f * (float)(c / COLUMNS) + 1.0f) / ROWS;
		m[14] = 0.5f;
		m[15] = 1.0f;
		const float shade = 0.5f + 0.5f * (float)(c % 7) / 6.0f;
		tints[c][0] = tints[c][1] = tints[c][2] = shade;
		tints[c][3] = 1.0f;
	}
}

// Each cube's two constant groups in memory, the vertex shader's matrix then the pixel shader's tint and three spare
// vectors, for the variant that loads them.
static bool lay_out_loaded(void)
{
	loaded = xsdk_gpu_alloc(LARGE * 2 * GROUP_BYTES);
	if (!loaded)
		return false;
	for (uint32_t c = 0; c < LARGE; c++) {
		volatile uint32_t *const group = &loaded[c * 2 * GROUP_FLOATS];
		for (uint32_t i = 0; i < GROUP_FLOATS; i++) {
			uint32_t matrix;
			uint32_t tint = 0;
			memcpy(&matrix, &mvps[c][i], sizeof matrix);
			if (i < 4)
				memcpy(&tint, &tints[c][i], sizeof tint);
			group[i] = matrix;
			group[GROUP_FLOATS + i] = tint;
		}
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(loaded), LARGE * 2 * GROUP_BYTES);
	return true;
}

static bool make_textures(void)
{
	static const uint8_t colours[TEXTURES][2][3] = {
		// Red over blue in both of the first's colours, blue over red in both of the second's, so any sample says
		// which texture it came from.
		{ { 224, 80, 48 }, { 200, 120, 40 } },
		{ { 48, 160, 224 }, { 40, 60, 200 } },
	};
	static uint8_t pixels[TEXTURE_SIZE * TEXTURE_SIZE * 4];
	for (uint32_t t = 0; t < TEXTURES; t++) {
		if (!NT_SUCCESS(xsdk_texture_create(&textures[t], XSDK_FORMAT_R8G8B8A8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true)))
			return false;
		for (uint32_t y = 0; y < TEXTURE_SIZE; y++) {
			for (uint32_t x = 0; x < TEXTURE_SIZE; x++) {
				const uint8_t *const c = colours[t][((x / TEXTURE_CHECKER) ^ (y / TEXTURE_CHECKER)) & 1];
				uint8_t *const out = &pixels[(y * TEXTURE_SIZE + x) * 4];
				memcpy(out, c, 3);
				out[3] = 255;
			}
		}
		xsdk_texture_upload(&textures[t], 0, pixels, TEXTURE_SIZE * 4);
		xsdk_gpu_invalidate(textures[t].address, textures[t].size_bytes);
	}
	return true;
}

// vkbench's: the position and texture coordinates, which xost reads as the position and colour usages
// (xost:xsc/resolve.py:158-162).
static bool load_shaders(void)
{
	const XsdkVertexElement layout[] = {
		{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT3, .offset_bytes = 0 },
		{ .usage = XSDK_USAGE_COLOR, .format = XSDK_VERTEX_FLOAT2, .offset_bytes = 12 },
	};
	const uint32_t strides[XSDK_VERTEX_STREAMS] = { VERTEX_BYTES };
	return NT_SUCCESS(xsdk_shader_load(&vertex_shader, cube_vertex.container, cube_vertex.container_bytes)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel_shaders[0], cube_pixel.container, cube_pixel.container_bytes)) &&
		NT_SUCCESS(xsdk_shader_load(&pixel_shaders[1], cube_gray_pixel.container, cube_gray_pixel.container_bytes)) &&
		NT_SUCCESS(
			xsdk_shader_load(&plain_vertex_shader, cube_plain_vertex.container, cube_plain_vertex.container_bytes)) &&
		NT_SUCCESS(xsdk_shader_load(&flat_pixel_shader, cube_flat_pixel.container, cube_flat_pixel.container_bytes)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 2, strides)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(&plain_vertex_shader, layout, 1, strides));
}

static void make_state(void)
{
	const XsdkSampler setting = { .mag_filter = XSDK_FILTER_LINEAR,
		.min_filter = XSDK_FILTER_LINEAR,
		.mip_filter = XSDK_FILTER_POINT,
		.address_u = XSDK_ADDRESS_WRAP,
		.address_v = XSDK_ADDRESS_WRAP };
	xsdk_texture_sampler_state(&sampler, &setting);
	const XsdkBlend opaque_blend = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	const XsdkBlend alpha_blend = { XSDK_BLEND_SRC_ALPHA, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD,
		XSDK_BLEND_ONE, XSDK_BLEND_ONE_MINUS_SRC_ALPHA, XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&opaque, &opaque_blend);
	xsdk_gpu_blend_state(&alpha, &alpha_blend);
	const XsdkBlend no_colour_blend = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE,
		XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, 0 };
	xsdk_gpu_blend_state(&no_colour, &no_colour_blend);
	const XsdkDepthStencil depth = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	xsdk_gpu_depth_stencil_state(&depth_test, &depth);
	const XsdkDepthStencil none = { .depth_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&no_depth, &none);
	const XsdkRaster setting_raster = { .cull = XSDK_CULL_NONE };
	xsdk_gpu_raster_state(&raster, &setting_raster);
	xsdk_gpu_set_depth_clear(1.0f, 0);
}

static void write_literals(const DrawCostShader *shader, bool vertex)
{
	for (uint32_t i = 0; i < shader->literal_count; i++) {
		const DrawCostLiteral *const literal = &shader->literals[i];
		if (vertex)
			xsdk_gpu_set_vertex_constants(literal->reg, literal->values, 1);
		else
			xsdk_gpu_set_pixel_constants(literal->reg, literal->values, 1);
	}
}

// As vkbench's backend uses a program: the shaders, then their literals.
static void use_shaders(const XsdkShader *vertex, const DrawCostShader *vertex_source, const XsdkShader *pixel,
	const DrawCostShader *pixel_source)
{
	xsdk_gpu_set_shaders(vertex, pixel);
	write_literals(vertex_source, true);
	write_literals(pixel_source, false);
}

static void use_program(bool gray)
{
	use_shaders(&vertex_shader, &cube_vertex, &pixel_shaders[gray], gray ? &cube_gray_pixel : &cube_pixel);
}

static void set_constants(uint32_t c)
{
	xsdk_gpu_set_vertex_constants(cube_vertex.push_register, mvps[c], MVP_VECTORS);
	xsdk_gpu_set_pixel_constants(cube_pixel.push_register + TINT_OFFSET_REGISTERS, tints[c], 1);
}

static void draw(void)
{
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, CUBE_INDICES, false);
}

// The frame's state, as vkbench's backend sets it each frame, with the first cube's constants for the variants that
// set none of their own.
static void set_state(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_viewport(-0.5f, HEIGHT - 0.5f, WIDTH, -(float)HEIGHT, 0.0f, 1.0f);
	xsdk_gpu_set_scissor(0, 0, WIDTH, HEIGHT);
	xsdk_gpu_set_raster(&raster);
	xsdk_gpu_set_depth_stencil(&depth_test);
	xsdk_gpu_set_blend(&opaque);
	use_program(false);
	xsdk_gpu_set_texture(cube_pixel.sampler, &textures[0], &sampler);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices), CUBE_VERTICES * VERTEX_BYTES);
	set_constants(0);
}

static void record_cubes(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		set_constants(c);
		draw();
	}
}

static void record_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		draw();
}

static void record_vertex_constants(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		xsdk_gpu_set_vertex_constants(cube_vertex.push_register, mvps[c], MVP_VECTORS);
		draw();
	}
}

static void record_pixel_constants(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		xsdk_gpu_set_pixel_constants(cube_pixel.push_register + TINT_OFFSET_REGISTERS, tints[c], 1);
		draw();
	}
}

static void record_constants_only(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		set_constants(c);
}

static void record_loaded(uint32_t count)
{
	const uint32_t base = xsdk_gpu_address(loaded);
	for (uint32_t c = 0; c < count; c++) {
		xsdk_gpu_load_vertex_constants(cube_vertex.push_register, base + c * 2 * GROUP_BYTES, GROUP_VECTORS);
		xsdk_gpu_load_pixel_constants(
			cube_pixel.push_register + TINT_OFFSET_REGISTERS, base + c * 2 * GROUP_BYTES + GROUP_BYTES, GROUP_VECTORS);
		draw();
	}
}

static void record_texture_changes(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		xsdk_gpu_set_texture(cube_pixel.sampler, &textures[c & 1], &sampler);
		set_constants(c);
		draw();
	}
}

static void record_shader_changes(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		use_program(c & 1);
		set_constants(c);
		draw();
	}
}

// vkbench's states scene: the texture every draw, the shader every second and blending every fourth.
static void record_states(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		if ((c & 1) == 0)
			use_program((c >> 1) & 1);
		xsdk_gpu_set_texture(cube_pixel.sampler, &textures[c & 1], &sampler);
		if ((c & 3) == 0)
			xsdk_gpu_set_blend((c >> 2) & 1 ? &alpha : &opaque);
		set_constants(c);
		draw();
	}
}

// The cube's 36 corners drawn in order, no indices: its vertex buffer is set once a batch, which both batches share.
static void record_auto_draws(uint32_t count)
{
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(unrolled), CUBE_INDICES * VERTEX_BYTES);
	for (uint32_t c = 0; c < count; c++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, CUBE_INDICES);
}

static void record_triangle_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, 3, false);
}

static void record_doubled_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, doubled_address, CUBE_INDICES * 2, false);
}

// The program in use set again, unchanged, as the shader-change variant sets a new one.
// The shaders set again, unchanged, without writing their literals again.
static void record_same_shaders(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		xsdk_gpu_set_shaders(&vertex_shader, &pixel_shaders[0]);
		set_constants(c);
		draw();
	}
}

// The texture's fetch constant group alone, without the flush xsdk_gpu_set_texture writes after it (src/gpu_draw.c).
static void put_texture_unflushed(const XsdkTexture *texture)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + FETCH_GROUP_DWORDS);
	packet[0] = (FETCH_GROUP_DWORDS - 1) << 16 | (SHADER_CONSTANT_FETCH_00_0 + cube_pixel.sampler * FETCH_GROUP_DWORDS);
	for (uint32_t i = 0; i < FETCH_GROUP_DWORDS; i++)
		packet[1 + i] = texture->fetch[i] | sampler.fetch[i];
}

static void record_texture_changes_unflushed(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		put_texture_unflushed(&textures[c & 1]);
		set_constants(c);
		draw();
	}
}

static void record_same_program(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		use_program(false);
		set_constants(c);
		draw();
	}
}

static void record_flat_draws(uint32_t count)
{
	use_shaders(&vertex_shader, &cube_vertex, &flat_pixel_shader, &cube_flat_pixel);
	record_draws(count);
}

static void record_plain_draws(uint32_t count)
{
	use_shaders(&plain_vertex_shader, &cube_plain_vertex, &flat_pixel_shader, &cube_flat_pixel);
	record_draws(count);
}

static void record_plain_triangles(uint32_t count)
{
	use_shaders(&plain_vertex_shader, &cube_plain_vertex, &flat_pixel_shader, &cube_flat_pixel);
	record_triangle_draws(count);
}

static void record_degenerate_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, degenerate_address, 3, false);
}

static void record_point_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_POINT_LIST, 1);
}

static void record_draws_no_depth(uint32_t count)
{
	xsdk_gpu_set_depth_stencil(&no_depth);
	record_draws(count);
}

static void record_draws_no_colour(uint32_t count)
{
	xsdk_gpu_set_blend(&no_colour);
	record_draws(count);
}

// The pixel shader loaded alone, the vertex shader left as it is, then the program registers, as
// xsdk_gpu_set_shaders writes them for the pair.
static void put_pixel_shader(const XsdkShader *pixel)
{
	const uint32_t context_misc =
		((vertex_shader.context_misc | pixel->context_misc) & ~(uint32_t)SAMPLE_CNTL_MASK) | SAMPLE_CNTL_CENTERS_ONLY;
	volatile uint32_t *const packet = xsdk_gpu_reserve(3 + 1 + PROGRAM_REGISTERS);
	packet[0] = xsdk_gpu_type3(PM4_IM_LOAD, 2);
	packet[1] = pixel->address | pixel->type;
	packet[2] = pixel->dword_count;
	packet[3] = (PROGRAM_REGISTERS - 1) << 16 | SQ_PROGRAM_CNTL;
	packet[4] = vertex_shader.program_control | pixel->program_control;
	packet[5] = context_misc;
	packet[6] = INTERPOLATE_ALL_AT_CENTERS;
}

static void record_pixel_shader_reloads(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		put_pixel_shader(&pixel_shaders[0]);
		set_constants(c);
		draw();
	}
}

static void record_pixel_shader_changes(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		put_pixel_shader(&pixel_shaders[c & 1]);
		write_literals(c & 1 ? &cube_gray_pixel : &cube_pixel, false);
		set_constants(c);
		draw();
	}
}

// The DRAW_INDX packet alone, without the COHER_DEST_BASE_1 and VGT_INDX_OFFSET writes the layer puts before it, which
// hold the 0 the layer's draws in the frame's state left them.
static void record_bare_draws(uint32_t count)
{
	for (uint32_t c = 0; c < count; c++) {
		set_constants(c);
		volatile uint32_t *const packet = xsdk_gpu_reserve(5);
		packet[0] = xsdk_gpu_type3(PM4_DRAW_INDX, 4);
		packet[1] = 0;
		packet[2] = XSDK_PRIMITIVE_TRIANGLE_LIST | CUBE_INDICES << VERTEX_COUNT_SHIFT;
		packet[3] = index_address;
		packet[4] = CUBE_INDICES | DMA_SWAP_8IN16;
	}
}

// The ones that only leave things out or add the layer's own calls first; the one whose packets the layer never
// writes last, so a hang there leaves the rest timed.
static const Variant variants[] = {
	{ "a cube as the layer draws it: both constant groups and the draw", record_cubes },
	{ "the draw alone", record_draws },
	{ "the vertex constants and the draw", record_vertex_constants },
	{ "the pixel constants and the draw", record_pixel_constants },
	{ "both constant groups, no draw", record_constants_only },
	{ "both constant groups loaded from memory, and the draw", record_loaded },
	{ "a cube and a texture change", record_texture_changes },
	{ "a cube and a shader change", record_shader_changes },
	{ "vkbench's states: a texture change every draw, a shader change every second, blending every fourth",
		record_states },
	{ "the draw alone, its 36 corners without indices", record_auto_draws },
	{ "the draw alone, one triangle", record_triangle_draws },
	{ "the draw alone, the cube's indices twice over", record_doubled_draws },
	{ "a cube and its program set again, unchanged", record_same_program },
	{ "a cube and its shaders set again, unchanged, not their literals", record_same_shaders },
	{ "the draw alone, the pixel shader without its texture", record_flat_draws },
	{ "the draw alone, both shaders without texture coordinates", record_plain_draws },
	{ "one triangle, both shaders without texture coordinates", record_plain_triangles },
	{ "a cube and its pixel shader loaded again, the vertex shader left", record_pixel_shader_reloads },
	{ "a cube and a pixel shader change, the vertex shader left", record_pixel_shader_changes },
	{ "a cube and a texture change without the fetch constant flush", record_texture_changes_unflushed },
	{ "one triangle with no area", record_degenerate_draws },
	{ "one point", record_point_draws },
	{ "the draw alone, no depth test", record_draws_no_depth },
	{ "the draw alone, no colour written", record_draws_no_colour },
	{ "a cube, the draw packet without its two register writes", record_bare_draws },
};

enum { VARIANTS = sizeof variants / sizeof variants[0] };

// Each variant's two batches, and their best times in ticks.
static XsdkCommandBuffer buffers[VARIANTS][SIZES];
static uint64_t best[VARIANTS][SIZES];

static const uint32_t sizes[SIZES] = { SMALL, LARGE };

static bool record_all(void)
{
	for (uint32_t v = 0; v < VARIANTS; v++) {
		for (uint32_t s = 0; s < SIZES; s++) {
			if (!NT_SUCCESS(xsdk_gpu_command_buffer_create(&buffers[v][s], sizes[s] * MAX_DRAW_DWORDS)))
				return false;
			xsdk_gpu_record_begin(&buffers[v][s]);
			variants[v].record(sizes[s]);
			if (!NT_SUCCESS(xsdk_gpu_record_end()))
				return false;
		}
	}
	return true;
}

// eDRAM cleared through a resolve, the state set and the GPU idle; then the batch, timed from its kick to its fence.
static bool time_batch(const XsdkCommandBuffer *buffer, uint64_t *ticks)
{
	xsdk_gpu_resolve(xsdk_gpu_address(offscreen), WIDTH, HEIGHT, CLEAR_ARGB);
	set_state();
	if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
		return false;
	const uint64_t start = time_base();
	xsdk_gpu_execute(buffer);
	const uint32_t fence = xsdk_gpu_fence();
	while (!xsdk_gpu_fence_done(fence)) {
		if (time_base() - start > (uint64_t)FENCE_TIMEOUT_MS * TICKS_PER_MS)
			return false;
	}
	*ticks = time_base() - start;
	return true;
}

typedef struct {
	uint32_t drawn;
	uint32_t right_texture;
} Look;

// After a variant's large batch, the first cells' centres: how many show a cube rather than the clear colour, and how
// many the texture the cube set, red for the first and blue for the second; alternating, each second cube set the
// second.
static Look look(uint32_t variant, bool alternating)
{
	Look seen = { 0, 0 };
	uint64_t ticks;
	if (!time_batch(&buffers[variant][1], &ticks))
		return seen;
	xsdk_gpu_resolve(xsdk_gpu_address(offscreen), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS)))
		return seen;
	for (uint32_t c = 0; c < CHECKED_CELLS; c++) {
		const uint32_t x = (c % COLUMNS) * WIDTH / COLUMNS + WIDTH / COLUMNS / 2;
		const uint32_t y = (c / COLUMNS) * HEIGHT / ROWS + HEIGHT / ROWS / 2;
		// Bytes B, G, R, A.
		const uint32_t pixel = offscreen[tiled_index(x, y, WIDTH)];
		const uint32_t red = pixel >> 8 & 0xFF;
		const uint32_t blue = pixel >> 24;
		seen.drawn += (pixel >> 8) != (CLEAR_ARGB & 0x00FFFFFF);
		seen.right_texture += (alternating && (c & 1)) ? blue > red : red > blue;
	}
	return seen;
}

static uint32_t variant_of(void (*record)(uint32_t))
{
	for (uint32_t v = 0; v < VARIANTS; v++) {
		if (variants[v].record == record)
			return v;
	}
	return 0;
}

static double microseconds(uint64_t ticks)
{
	return (double)ticks * 1000.0 / TICKS_PER_MS;
}

int main(void)
{
	xsdk_log_write("gpu draw cost probe: %u variants, batches of %u and %u cubes, best of %u rounds",
		(unsigned)VARIANTS, (unsigned)SMALL, (unsigned)LARGE, (unsigned)ROUNDS);
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("gpu draw cost probe: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	place_cubes();
	offscreen = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!offscreen || !make_cube() || !lay_out_loaded() || !make_textures() || !load_shaders()) {
		xsdk_log_write("gpu draw cost probe: the buffers, textures or shaders couldn't be made");
		return 1;
	}
	make_state();
	set_state();
	if (!record_all()) {
		xsdk_log_write("gpu draw cost probe: a batch couldn't be recorded");
		return 1;
	}
	const Look cubes = look(0, false);
	const Look flushed = look(variant_of(record_texture_changes), true);
	const Look unflushed = look(variant_of(record_texture_changes_unflushed), true);
	xsdk_log_write(
		"gpu draw cost probe: of the first %u cells, %u show their cube; %u the texture each cube set, with "
		"the fetch constant flush; %u without it",
		(unsigned)CHECKED_CELLS, (unsigned)cubes.drawn, (unsigned)flushed.right_texture,
		(unsigned)unflushed.right_texture);
	const bool drawn =
		cubes.drawn >= CHECKED_DRAWN && cubes.right_texture >= CHECKED_DRAWN && flushed.right_texture >= CHECKED_DRAWN;
	for (uint32_t v = 0; v < VARIANTS; v++) {
		xsdk_log_write("gpu draw cost probe: timing %s", variants[v].name);
		for (uint32_t round = 0; round < ROUNDS; round++) {
			for (uint32_t s = 0; s < SIZES; s++) {
				uint64_t ticks;
				if (!time_batch(&buffers[v][s], &ticks)) {
					xsdk_log_write("gpu draw cost probe: %s: the GPU didn't finish %u cubes", variants[v].name,
						(unsigned)sizes[s]);
					return 1;
				}
				if (round == 0 || ticks < best[v][s])
					best[v][s] = ticks;
			}
		}
		const double small = microseconds(best[v][0]);
		const double large = microseconds(best[v][1]);
		xsdk_log_write("gpu draw cost probe: %s: %.3f us a cube; %.1f us for %u, %.1f for %u", variants[v].name,
			(large - small) / (LARGE - SMALL), small, (unsigned)SMALL, large, (unsigned)LARGE);
	}
	status = xsdk_gpu_close();
	xsdk_log_write("gpu draw cost probe: done, %s; xsdk_gpu_close returned 0x%08X",
		drawn ? "the cubes drew" : "the cubes didn't draw", (unsigned)status);
	return drawn && NT_SUCCESS(status) ? 0 : 1;
}
