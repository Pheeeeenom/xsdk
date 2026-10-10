#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// Draws milestone 6's triangle with one thing changed at a time, logging how many pixels each draw covered.

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_MS = 1000,
	HOLD_MS = 3000,
	CLEAR_ARGB = 0xFF202020,
	// The display's magenta as a word in memory; a resolve that writes nothing leaves it.
	MAGENTA = 0xFF00FFFF,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	SLOT_DWORDS = 3,
	FIRST_FETCH_SLOT = 3,
	FETCHES = 2,
	// A vertex fetch's const_index and const_index_sel (xenia:src/xenia/gpu/ucode.h:753-754).
	FETCH_CONSTANT_BITS = 0x7F << 20,
	VERTEX_FETCH_CONSTANT = 95,
};

// Register indices and opcodes (xenia:src/xenia/gpu/register_table.inc, xenos.h:1572-1622).
enum {
	COHER_SIZE_HOST = 0x0A2F,
	COHER_STATUS_HOST = 0x0A31,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_CL_VTE_CNTL = 0x2206,
	SQ_PROGRAM_CNTL = 0x2180,
	SHADER_CONSTANT_FETCH_00_0 = 0x4800,
	SHADER_CONSTANT_FLUSH_FETCH_0 = 0x5000,
	PM4_DRAW_INDX = 0x22,
	PM4_IM_LOAD_IMMEDIATE = 0x2B,
	PM4_WAIT_REG_MEM = 0x3C,
};

// Values as the SDK and the resolve write them (src/gpu_draw.c, src/gpu_resolve.c).
enum {
	CLIP_D3D_SPACE = 0x00080000,
	CLIP_DISABLE = 1 << 16,
	VTE_PRE_TRANSFORMED = 0x300,
	INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF,
	TRIANGLE_LIST_AUTO_INDEX_3 = 4 | 2 << 6 | 3 << 16,
	VERTEX_FETCH_TYPE = 3,
	VERTEX_FETCH_8IN32 = 1 << 28 | 2,
	COHER_VERTEX_CACHE = 1 << 24,
	COHER_BUSY = 0x80000000,
};

typedef enum {
	CLIP_SPACE_Z_0,
	CLIP_SPACE_Z_HALF,
	SCREEN_SPACE,
	VERTEX_SETS,
} VertexSet;

typedef struct {
	const char *name;
	VertexSet vertex_set;
	bool clip_disabled;
	bool pre_transformed;
	bool draw_indx;
	bool fetch_constant_0;
} Variant;

static const Variant variants[] = {
	{ "as the SDK draws", CLIP_SPACE_Z_0, false, false, false, false },
	{ "z 0.5", CLIP_SPACE_Z_HALF, false, false, false, false },
	{ "clipping off", CLIP_SPACE_Z_0, true, false, false, false },
	{ "DRAW_INDX with a viz query token", CLIP_SPACE_Z_0, false, false, true, false },
	{ "vertex fetch constant 0", CLIP_SPACE_Z_0, false, false, false, true },
	{ "screen-space corners, no viewport transform or clipping, as the resolve draws", SCREEN_SPACE, true, true, false,
		false },
};

// Position, then colour: red at the top, green bottom left, blue bottom right.
static const float triangles[VERTEX_SETS][3][VERTEX_FLOATS] = {
	{
		{ 0.0f, 0.75f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
		{ -0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
		{ 0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
	},
	{
		{ 0.0f, 0.75f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
		{ -0.75f, -0.75f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
		{ 0.75f, -0.75f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
	},
	{
		{ 320.0f, 60.0f, 0.5f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
		{ 80.0f, 420.0f, 0.5f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
		{ 560.0f, 420.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
	},
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

// Inline, as the resolve loads its shader on the console.
static void put_shader(const XsdkShader *shader)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(3 + shader->dword_count);
	packet[0] = xsdk_gpu_type3(PM4_IM_LOAD_IMMEDIATE, 2 + shader->dword_count);
	packet[1] = shader->type;
	packet[2] = shader->dword_count;
	for (uint32_t i = 0; i < shader->dword_count; i++)
		packet[3 + i] = shader->microcode[i];
}

static void load_shaders(const XsdkShader *vertex, const XsdkShader *pixel)
{
	put_shader(pixel);
	put_shader(vertex);
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
	packet[0] = 2 << 16 | SQ_PROGRAM_CNTL;
	packet[1] = vertex->program_control | pixel->program_control;
	packet[2] = vertex->context_misc | pixel->context_misc;
	packet[3] = INTERPOLATE_ALL_AT_CENTERS;
}

// Points the vertex fetches at constant 0 or 95 and fills that constant, as xsdk_gpu_set_vertices does for 95.
static void set_vertices(const XsdkShader *vertex, uint32_t address, uint32_t size_bytes, bool fetch_constant_0)
{
	const uint32_t constant = fetch_constant_0 ? 0 : VERTEX_FETCH_CONSTANT;
	for (uint32_t i = 0; i < FETCHES; i++) {
		volatile uint32_t *const fetch = &vertex->microcode[(FIRST_FETCH_SLOT + i) * SLOT_DWORDS];
		fetch[0] = (fetch[0] & ~FETCH_CONSTANT_BITS) | (constant / 3) << 20 | (constant % 3) << 25;
	}
	__asm__ volatile("sync" ::: "memory");
	volatile uint32_t *const packet = xsdk_gpu_reserve(11 + 3 + 4);
	packet[0] = COHER_STATUS_HOST;
	packet[1] = COHER_VERTEX_CACHE;
	packet[2] = 1 << 16 | COHER_SIZE_HOST;
	packet[3] = size_bytes;
	packet[4] = address;
	packet[5] = xsdk_gpu_type3(PM4_WAIT_REG_MEM, 5);
	packet[6] = 3;
	packet[7] = COHER_STATUS_HOST;
	packet[8] = 0;
	packet[9] = COHER_BUSY;
	packet[10] = 8;
	packet[11] = 1 << 16 | (SHADER_CONSTANT_FETCH_00_0 + constant * 2);
	packet[12] = address | VERTEX_FETCH_TYPE;
	packet[13] = VERTEX_FETCH_8IN32 | size_bytes / 4 << 2;
	packet[14] = 2 << 16 | SHADER_CONSTANT_FLUSH_FETCH_0;
	packet[15] = 0;
	packet[16] = 0;
	packet[17] = 0;
}

static void draw(bool draw_indx)
{
	if (!draw_indx) {
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
		return;
	}
	// xenia:src/xenia/gpu/command_processor.cc:1406-1416
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_DRAW_INDX, 2);
	packet[1] = 0;
	packet[2] = TRIANGLE_LIST_AUTO_INDEX_3;
}

static void count(volatile const uint32_t *pixels, uint32_t *drawn, uint32_t *unwritten, uint32_t *first)
{
	const uint32_t clear = __builtin_bswap32(CLEAR_ARGB);
	*drawn = 0;
	*unwritten = 0;
	*first = 0;
	for (uint32_t i = 0; i < PIXELS; i++) {
		const uint32_t pixel = pixels[i];
		if (pixel == MAGENTA) {
			(*unwritten)++;
		} else if (pixel != clear) {
			if (!*drawn)
				*first = pixel;
			(*drawn)++;
		}
	}
}

static void draw_variant(const Variant *variant, const XsdkShader *vertex, const XsdkShader *pixel,
	const uint32_t *vertex_sets, volatile uint32_t *buffer, uint32_t scratch, bool has_display)
{
	for (uint32_t i = 0; i < PIXELS; i++)
		buffer[i] = MAGENTA;
	__asm__ volatile("sync" ::: "memory");
	const uint32_t address = xsdk_gpu_address(buffer);
	// A resolve and a fence first, so eDRAM holds only the clear colour and the GPU is idle.
	xsdk_gpu_resolve(scratch, WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS cleared = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	put_register(PA_CL_CLIP_CNTL, variant->clip_disabled ? CLIP_D3D_SPACE | CLIP_DISABLE : CLIP_D3D_SPACE);
	if (variant->pre_transformed)
		put_register(PA_CL_VTE_CNTL, VTE_PRE_TRANSFORMED);
	set_vertices(vertex, vertex_sets[variant->vertex_set], sizeof triangles[0], variant->fetch_constant_0);
	load_shaders(vertex, pixel);
	draw(variant->draw_indx);
	const NTSTATUS drawn_status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS resolved = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	uint32_t drawn = 0;
	uint32_t unwritten = 0;
	uint32_t first = 0;
	count(buffer, &drawn, &unwritten, &first);
	xsdk_log_write("triangle: %s: fences 0x%08X 0x%08X 0x%08X; drew %u pixels, the first %08X; %u left magenta",
		variant->name, (unsigned)cleared, (unsigned)drawn_status, (unsigned)resolved, (unsigned)drawn, (unsigned)first,
		(unsigned)unwritten);
	if (!has_display)
		return;
	const NTSTATUS shown = xsdk_display_show(address, WIDTH);
	xsdk_log_write("triangle: showing it for %u ms: 0x%08X", (unsigned)HOLD_MS, (unsigned)shown);
	(void)xsdk_thread_sleep(HOLD_MS);
}

static bool upload_triangles(uint32_t *vertex_sets)
{
	for (uint32_t set = 0; set < VERTEX_SETS; set++) {
		volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof triangles[set]);
		if (!vertices)
			return false;
		uint32_t words[sizeof triangles[set] / 4];
		memcpy(words, triangles[set], sizeof triangles[set]);
		for (uint32_t i = 0; i < sizeof triangles[set] / 4; i++)
			vertices[i] = words[i];
		vertex_sets[set] = xsdk_gpu_address(vertices);
	}
	__asm__ volatile("sync" ::: "memory");
	return true;
}

int main(void)
{
	xsdk_log_write("triangle probe");
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("triangle: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	uint32_t width = 0;
	uint32_t height = 0;
	const NTSTATUS display_status = xsdk_display_open(&width, &height);
	const bool has_display = NT_SUCCESS(display_status) && width == WIDTH && height == HEIGHT;
	xsdk_log_write("triangle: display 0x%08X, %ux%u", (unsigned)display_status, (unsigned)width, (unsigned)height);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	uint32_t vertex_sets[VERTEX_SETS];
	volatile uint32_t *const buffer = xsdk_gpu_alloc(PIXELS * 4);
	volatile uint32_t *const scratch = xsdk_gpu_alloc(PIXELS * 4);
	if (!NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, sizeof layout / sizeof layout[0], STRIDE_BYTES)) ||
		!upload_triangles(vertex_sets) || !buffer || !scratch)
		return 1;
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		draw_variant(&variants[i], &vertex, &pixel, vertex_sets, buffer, xsdk_gpu_address(scratch), has_display);
	xsdk_display_close();
	xsdk_log_write("triangle: done; the GPU goes back at exit");
	return 0;
}
