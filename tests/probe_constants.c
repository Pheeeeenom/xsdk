#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "constant_shaders.h"
#include "probe_constant_shaders.h"
#include "probe_gpu.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Steps from no constants, through each shader feature on constants written libxenon's way, to the test's writes,
// then float constants in smaller groups than libxenon's 16 (milestone 19, step 3).

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	MULTI_FRAMES = 30,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 4,
	QUAD_VERTICES = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 2,
	LOOP_STEP = 0x40,
	// A type-2 packet, one dword that does nothing, as libxenon pads with (libxenon:drivers/xenos/xe.c:1217-1221).
	TYPE2_NOP = 0x80000000,
	// libxenon uploads float constants 16 at a time, and the 8 boolean words and 32 loops as one block
	// (xe.c:1320-1369).
	FLOAT_BLOCK = 16,
	INTEGER_BLOCK_DWORDS = 40,
	BOOL_WORD = 4,
	SHADER_CONSTANT_000_X = 0x4000,
	SHADER_CONSTANT_256_X = 0x4400,
	SHADER_CONSTANT_BOOL_000_031 = 0x4900,
	SHADER_CONSTANT_LOOP_00 = 0x4908,
	SQ_VS_CONST = 0x2307,
	SQ_PS_CONST = 0x2308,
	// Writes constants from its payload; index 0 of type 0 is register 0x4000 (xenia:src/xenia/gpu/xenos.h:1607,
	// command_processor.cc:1428-1462).
	PM4_SET_CONSTANT = 0x2D,
	SET_CONSTANT_ALU = 0,
};

typedef enum {
	WRITES_NONE,
	// Aligned whole blocks, as libxenon writes them.
	WRITES_LIBXENON,
	// Aligned, but only the three float constants and one boolean word and one loop constant.
	WRITES_ALIGNED_PARTIAL,
	// The SDK's calls as test_constants makes them: partial and wherever the ring is.
	WRITES_SDK,
	// Float constants alone in smaller aligned groups, each stage's c0 to c2, the integers still one whole block.
	WRITES_FLOATS_4,
	WRITES_FLOATS_3,
	WRITES_FLOATS_1,
	// The same through SET_CONSTANT, its values aligned as libxenon aligns them.
	WRITES_SET_CONSTANT_4,
	WRITES_SET_CONSTANT_1,
} Writes;

typedef struct {
	const char *name;
	const uint32_t *vertex;
	uint32_t vertex_bytes;
	const uint32_t *pixel;
	uint32_t pixel_bytes;
	Writes writes;
	// Placed by c0 and c1, or drawn where the unit square's clip-space coordinates put it.
	bool placed;
	bool use_bool;
	uint8_t loop_passes;
	// One quad, or test_constants's six.
	bool six;
	uint32_t frames;
	// What the quad's centre should hold, or 0 to accept anything but the clear colour.
	uint32_t want_argb;
} Step;

typedef struct {
	uint32_t x;
	uint32_t y;
	uint8_t red;
	uint8_t green;
	bool use_bool;
	uint8_t bool_green;
	uint8_t loop_passes;
} Quad;

// test_constants's six quads, 320x240 each.
static const Quad six_quads[] = {
	{ 80, 80, 0xC0, 0x00, false, 0x00, 0 },
	{ 480, 80, 0x00, 0xC0, false, 0x00, 0 },
	{ 880, 80, 0x40, 0x20, true, 0xFF, 0 },
	{ 80, 400, 0x40, 0x40, false, 0x00, 1 },
	{ 480, 400, 0x00, 0x00, false, 0x00, 3 },
	{ 880, 400, 0xFF, 0x20, true, 0x80, 2 },
};

#define SHADER(name) name, sizeof name

static const Step steps[] = {
	{ "no constants", SHADER(probe_plain_vertex_shader), SHADER(triangle_pixel_shader), WRITES_NONE, false, false, 0,
		false, 1, 0 },
	{ "vertex shader reads c2", SHADER(probe_constant_vertex_shader), SHADER(triangle_pixel_shader), WRITES_LIBXENON,
		false, false, 0, false, 1, 0xFFC00000 },
	{ "vertex shader mad with c0 and c1", SHADER(constant_vertex_shader), SHADER(triangle_pixel_shader),
		WRITES_LIBXENON, true, false, 0, false, 1, 0xFFC00000 },
	{ "pixel shader reads c0", SHADER(constant_vertex_shader), SHADER(probe_constant_pixel_shader), WRITES_LIBXENON,
		true, false, 0, false, 1, 0xFFC0C000 },
	{ "cexec b128 with the boolean clear", SHADER(constant_vertex_shader), SHADER(probe_bool_pixel_shader),
		WRITES_LIBXENON, true, false, 0, false, 1, 0xFFC0C000 },
	{ "cexec b128 with the boolean set", SHADER(constant_vertex_shader), SHADER(probe_bool_pixel_shader),
		WRITES_LIBXENON, true, true, 0, false, 1, 0xFFC0FF00 },
	{ "loop i0, 0 passes", SHADER(constant_vertex_shader), SHADER(probe_loop_pixel_shader), WRITES_LIBXENON, true,
		false, 0, false, 1, 0xFFC0C000 },
	{ "loop i0, 1 pass", SHADER(constant_vertex_shader), SHADER(probe_loop_pixel_shader), WRITES_LIBXENON, true, false,
		1, false, 1, 0xFFC0C040 },
	{ "loop i0, 3 passes", SHADER(constant_vertex_shader), SHADER(probe_loop_pixel_shader), WRITES_LIBXENON, true,
		false, 3, false, 1, 0xFFC0C0C0 },
	{ "six quads, libxenon's writes", SHADER(constant_vertex_shader), SHADER(constant_pixel_shader), WRITES_LIBXENON,
		true, false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, the SDK's writes", SHADER(constant_vertex_shader), SHADER(constant_pixel_shader), WRITES_SDK, true,
		false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, float constants in groups of 4", SHADER(constant_vertex_shader), SHADER(constant_pixel_shader),
		WRITES_FLOATS_4, true, false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, float constants in groups of 4 through SET_CONSTANT", SHADER(constant_vertex_shader),
		SHADER(constant_pixel_shader), WRITES_SET_CONSTANT_4, true, false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, float constants one at a time", SHADER(constant_vertex_shader), SHADER(constant_pixel_shader),
		WRITES_FLOATS_1, true, false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, float constants one at a time through SET_CONSTANT", SHADER(constant_vertex_shader),
		SHADER(constant_pixel_shader), WRITES_SET_CONSTANT_1, true, false, 0, true, MULTI_FRAMES, 0 },
	{ "six quads, float constants in groups of 3", SHADER(constant_vertex_shader), SHADER(constant_pixel_shader),
		WRITES_FLOATS_3, true, false, 0, true, MULTI_FRAMES, 0 },
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

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static float unorm(uint8_t value)
{
	return value / 255.0f;
}

// Pads to libxenon's alignment unless told not to: the header 3 dwords into a 16-byte group, the values after it.
static void put_registers(uint32_t first_register, const uint32_t *values, uint32_t count, bool align)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(count + 4);
	uint32_t i = 0;
	while (align && ((uint32_t)(uintptr_t)&packet[i] >> 2 & 3) != 3)
		packet[i++] = TYPE2_NOP;
	packet[i++] = (count - 1) << 16 | first_register;
	for (uint32_t v = 0; v < count; v++)
		packet[i++] = values[v];
	while (i < count + 4)
		packet[i++] = TYPE2_NOP;
}

// SET_CONSTANT's values start on a 16-byte line, as put_registers aligns a type-0 packet's.
static void put_set_constant(uint32_t first_register, const uint32_t *values, uint32_t count)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(count + 5);
	uint32_t i = 0;
	while (((uint32_t)(uintptr_t)&packet[i] >> 2 & 3) != 2)
		packet[i++] = TYPE2_NOP;
	packet[i++] = xsdk_gpu_type3(PM4_SET_CONSTANT, count + 1);
	packet[i++] = SET_CONSTANT_ALU << 16 | (first_register - SHADER_CONSTANT_000_X);
	for (uint32_t v = 0; v < count; v++)
		packet[i++] = values[v];
	while (i < count + 5)
		packet[i++] = TYPE2_NOP;
}

// Each stage's c0 to c2 in groups of size registers, through type-0 packets or SET_CONSTANT.
static void put_float_groups(const uint32_t *vertex, const uint32_t *pixel, uint32_t size, bool set_constant)
{
	for (uint32_t first = 0; first < 3; first += size) {
		const uint32_t count = (first + size <= 3 || size == 4 ? size : 3 - first) * 4;
		if (set_constant) {
			put_set_constant(SHADER_CONSTANT_000_X + first * 4, &vertex[first * 4], count);
			put_set_constant(SHADER_CONSTANT_256_X + first * 4, &pixel[first * 4], count);
		} else {
			put_registers(SHADER_CONSTANT_000_X + first * 4, &vertex[first * 4], count, true);
			put_registers(SHADER_CONSTANT_256_X + first * 4, &pixel[first * 4], count, true);
		}
	}
}

// Edges half a pixel early, as test_constants places them.
static void quad_constants(const Quad *quad, uint32_t *vertex, uint32_t *pixel)
{
	const float left = (quad->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (quad->x + 320 - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (quad->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (quad->y + 240 - 0.5f) / (HEIGHT / 2.0f);
	const float vertex_values[12] = { right - left, bottom - top, 1.0f, 1.0f, left, top, 0.0f, 0.0f, unorm(quad->red),
		0.0f, 0.0f, 1.0f };
	const float pixel_values[12] = { 0.0f, unorm(quad->green), 0.0f, 0.0f, 0.0f, unorm(quad->bool_green), 0.0f, 0.0f,
		0.0f, 0.0f, unorm(LOOP_STEP), 0.0f };
	for (uint32_t i = 0; i < 12; i++) {
		vertex[i] = float_bits(vertex_values[i]);
		pixel[i] = float_bits(pixel_values[i]);
	}
}

static void write_constants(Writes writes, const Quad *quad)
{
	uint32_t vertex[FLOAT_BLOCK * 4] = { 0 };
	uint32_t pixel[FLOAT_BLOCK * 4] = { 0 };
	quad_constants(quad, vertex, pixel);
	uint32_t integers[INTEGER_BLOCK_DWORDS] = { 0 };
	integers[BOOL_WORD] = quad->use_bool;
	integers[SHADER_CONSTANT_LOOP_00 - SHADER_CONSTANT_BOOL_000_031] = xsdk_gpu_loop_constant(quad->loop_passes, 0, 1);
	if (writes == WRITES_LIBXENON) {
		put_registers(SHADER_CONSTANT_000_X, vertex, FLOAT_BLOCK * 4, true);
		put_registers(SHADER_CONSTANT_256_X, pixel, FLOAT_BLOCK * 4, true);
		put_registers(SHADER_CONSTANT_BOOL_000_031, integers, INTEGER_BLOCK_DWORDS, true);
	} else if (writes == WRITES_ALIGNED_PARTIAL) {
		put_registers(SHADER_CONSTANT_000_X, vertex, 12, true);
		put_registers(SHADER_CONSTANT_256_X, pixel, 12, true);
		put_registers(SHADER_CONSTANT_BOOL_000_031 + BOOL_WORD, &integers[BOOL_WORD], 1, true);
		put_registers(
			SHADER_CONSTANT_LOOP_00, &integers[SHADER_CONSTANT_LOOP_00 - SHADER_CONSTANT_BOOL_000_031], 1, true);
	} else if (writes >= WRITES_FLOATS_4) {
		static const uint32_t sizes[] = { [WRITES_FLOATS_4] = 4,
			[WRITES_FLOATS_3] = 3,
			[WRITES_FLOATS_1] = 1,
			[WRITES_SET_CONSTANT_4] = 4,
			[WRITES_SET_CONSTANT_1] = 1 };
		put_float_groups(vertex, pixel, sizes[writes], writes >= WRITES_SET_CONSTANT_4);
		put_registers(SHADER_CONSTANT_BOOL_000_031, integers, INTEGER_BLOCK_DWORDS, true);
	} else if (writes == WRITES_SDK) {
		float vertex_floats[12];
		float pixel_floats[12];
		memcpy(vertex_floats, vertex, sizeof vertex_floats);
		memcpy(pixel_floats, pixel, sizeof pixel_floats);
		const uint32_t loop = integers[SHADER_CONSTANT_LOOP_00 - SHADER_CONSTANT_BOOL_000_031];
		xsdk_gpu_set_vertex_constants(0, vertex_floats, 3);
		xsdk_gpu_set_pixel_constants(0, pixel_floats, 3);
		xsdk_gpu_set_bool_constants(BOOL_WORD, &integers[BOOL_WORD], 1);
		xsdk_gpu_set_loop_constants(0, &loop, 1);
	}
}

static bool close_enough(uint32_t stored, uint32_t want_argb)
{
	const uint32_t want = __builtin_bswap32(want_argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static uint32_t quad_argb(const Quad *quad)
{
	const uint32_t green = quad->use_bool ? quad->bool_green : quad->green;
	return 0xFF000000 | (uint32_t)quad->red << 16 | green << 8 | (uint32_t)(quad->loop_passes * LOOP_STEP);
}

// Draws a step's frames; false once a fence doesn't land, after which the GPU is wedged.
static bool run_step(
	uint32_t index, const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices, volatile uint32_t *buffer)
{
	const Step *const step = &steps[index];
	const Quad one = { 480, 240, 0xC0, 0xC0, step->use_bool, 0xFF, step->loop_passes };
	uint32_t wrong = 0;
	uint32_t first_wrong = 0;
	uint32_t last_centre = 0;
	for (uint32_t frame = 0; frame < step->frames; frame++) {
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(0, vertices, sizeof square);
		const uint32_t quads = step->six ? sizeof six_quads / sizeof six_quads[0] : 1;
		for (uint32_t q = 0; q < quads; q++) {
			write_constants(step->writes, step->six ? &six_quads[q] : &one);
			xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
		}
		xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
		const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
		if (!NT_SUCCESS(status)) {
			xsdk_log_write("constants probe: step %u, %s: frame %u's fence didn't land: 0x%08X", (unsigned)index + 1,
				step->name, (unsigned)frame, (unsigned)status);
			probe_report_cp("constants probe", "after the hang");
			return false;
		}
		for (uint32_t q = 0; q < quads; q++) {
			const Quad *const quad = step->six ? &six_quads[q] : &one;
			// Unplaced, the unit square covers the top right quarter of the target.
			const uint32_t x = step->placed ? quad->x + 160 : WIDTH * 3 / 4;
			const uint32_t y = step->placed ? quad->y + 120 : HEIGHT / 4;
			const uint32_t centre = buffer[tiled_index(x, y, WIDTH)];
			const uint32_t want = step->six ? quad_argb(quad) : step->want_argb;
			const bool right = want ? close_enough(centre, want) : centre != __builtin_bswap32(CLEAR_ARGB);
			if (!right && !wrong)
				first_wrong = centre;
			wrong += !right;
			last_centre = centre;
		}
	}
	xsdk_log_write("constants probe: step %u, %s: %u frames landed, %u centres wrong (first %08X), last centre %08X",
		(unsigned)index + 1, step->name, (unsigned)step->frames, (unsigned)wrong, (unsigned)first_wrong,
		(unsigned)last_centre);
	return true;
}

int main(void)
{
	xsdk_log_write("constants probe");
	xsdk_log_write("constants probe: XAM had SQ_VS_CONST %08X, SQ_PS_CONST %08X",
		(unsigned)probe_gpu_registers[SQ_VS_CONST], (unsigned)probe_gpu_registers[SQ_PS_CONST]);
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("constants probe: GPU 0x%08X; SQ_VS_CONST %08X, SQ_PS_CONST %08X", (unsigned)status,
		(unsigned)probe_gpu_registers[SQ_VS_CONST], (unsigned)probe_gpu_registers[SQ_PS_CONST]);
	if (!NT_SUCCESS(status))
		return 1;
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	volatile uint32_t *const buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertices || !buffer)
		return 1;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), sizeof square);
	for (uint32_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
		XsdkShader vertex = { .microcode = NULL };
		XsdkShader pixel = { .microcode = NULL };
		const bool ready = NT_SUCCESS(xsdk_shader_load(&vertex, steps[i].vertex, steps[i].vertex_bytes)) &&
			NT_SUCCESS(xsdk_shader_load(&pixel, steps[i].pixel, steps[i].pixel_bytes)) &&
			NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, 1, (const uint32_t[]){ STRIDE_BYTES }));
		if (!ready) {
			xsdk_log_write("constants probe: step %u, %s: shaders didn't load", (unsigned)i + 1, steps[i].name);
			return 1;
		}
		const bool landed = run_step(i, &vertex, &pixel, xsdk_gpu_address(vertices), buffer);
		xsdk_shader_free(&pixel);
		xsdk_shader_free(&vertex);
		// A wedged GPU keeps reading what it has; the exit's close keeps it and its memory.
		if (!landed)
			return 1;
	}
	xsdk_gpu_free(buffer);
	xsdk_gpu_free(vertices);
	xsdk_log_write("constants probe: every step landed");
	return 0;
}
