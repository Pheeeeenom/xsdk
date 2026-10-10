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

// Draws milestone 6's triangle with libxenon's whole frame, then with one of its differences from the SDK undone at a
// time, logging how many pixels each draw covered.

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
	VERTEX_FETCH_CONSTANT = 95,
	ALU_CONSTANT_DWORDS = 512 * 4,
	ALU_CONSTANTS_A_PACKET = 512,
	FETCH_CONSTANT_DWORDS = 96 * 2,
	CLIP_PLANE_DWORDS = 6 * 4,
	BOOL_AND_LOOP_DWORDS = 8 + 32,
};

// Register indices and opcodes (xenia:src/xenia/gpu/register_table.inc, xenos.h:1572-1622).
enum {
	COHER_SIZE_HOST = 0x0A2F,
	COHER_STATUS_HOST = 0x0A31,
	RB_SURFACE_INFO = 0x2000,
	COHER_DEST_BASE_1 = 0x2007,
	COHER_DEST_BASE_7 = 0x200D,
	PA_SC_WINDOW_OFFSET = 0x2080,
	VGT_MAX_VTX_INDX = 0x2100,
	VGT_INDX_OFFSET = 0x2102,
	RB_COLOR_MASK = 0x2104,
	RB_STENCILREFMASK_BF = 0x210C,
	SQ_PROGRAM_CNTL = 0x2180,
	RB_DEPTHCONTROL = 0x2200,
	PA_SC_AA_CONFIG = 0x2301,
	PA_SC_AA_MASK = 0x2312,
	PA_SU_POLY_OFFSET_FRONT_SCALE = 0x2380,
	PA_CL_UCP_0_X = 0x2388,
	SHADER_CONSTANT_000_X = 0x4000,
	SHADER_CONSTANT_FETCH_00_0 = 0x4800,
	SHADER_CONSTANT_BOOL_000_031 = 0x4900,
	SHADER_CONSTANT_FLUSH_FETCH_0 = 0x5000,
	PM4_DRAW_INDX = 0x22,
	PM4_IM_LOAD = 0x27,
	PM4_WAIT_REG_MEM = 0x3C,
	PREDICATED = 1,
};

// libxenon's values (libxenon:drivers/xenos/xe.c:1181-1185, 1403-1416, 1688-1744).
enum {
	DEPTH_LEQUAL_WRITE = 0x007007B6,
	BLEND_ONE_ZERO = 0x00010001,
	COLOR_CONTROL = 0x87000007,
	CLIP_D3D_SPACE = 0x00080000,
	SC_MODE_WINDOW_OFFSET_NO_CULL = 0x00010000,
	VTE_VIEWPORT_AND_W0 = 0x0000043F,
	EDRAM_MODE_COLOR_DEPTH = 4,
	SURFACE_INFO_640 = 640 | 640 << 18,
	COLOR_INFO_BASE_240 = 240,
	STENCIL_MASKS = 0x00FFFF00,
	INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF,
	MAX_VERTEX_INDEX = 0x00FFFFFF,
	VERTEX_FETCH_TYPE = 3,
	VERTEX_FETCH_8IN32 = 1 << 28 | 2,
	// The draw path's write after the fetch constants (xe.c:1702-1703).
	FLUSH_FETCH_1_VALUE = 0x00025000,
	TRIANGLE_LIST_AUTO_INDEX_3 = 4 | 2 << 6 | 3 << 16,
	COHER_VERTEX_CACHE = 1 << 24,
	COHER_BUSY = 0x80000000,
};

typedef struct {
	const char *name;
	bool sdk;
	bool size_in_bytes;
	bool depth_test;
	bool predicated_draw_indx;
	bool zero_bool_and_loop;
	bool zero_alu;
} Variant;

static const Variant variants[] = {
	{ "as libxenon draws", false, true, true, true, true, true },
	{ "as libxenon, but IM_LOAD's size in dwords", false, false, true, true, true, true },
	{ "as libxenon, but no depth test", false, true, false, true, true, true },
	{ "as libxenon, but DRAW_INDX_2", false, true, true, false, true, true },
	{ "as libxenon, but the bool and loop constants left alone", false, true, true, true, false, true },
	{ "as libxenon, but the ALU constants left alone", false, true, true, true, true, false },
	{ "as the SDK draws", true, false, false, false, false, false },
};

// Position in clip space, then colour: red at the top, green bottom left, blue bottom right.
static const float triangle[3][VERTEX_FLOATS] = {
	{ 0.0f, 0.75f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
	{ -0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

static void put_registers(uint32_t index, const uint32_t *values, uint32_t count)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + count);
	packet[0] = (count - 1) << 16 | index;
	for (uint32_t i = 0; i < count; i++)
		packet[1 + i] = values ? values[i] : 0;
}

static void put_register(uint32_t index, uint32_t value)
{
	put_registers(index, &value, 1);
}

static void put_zeros(uint32_t index, uint32_t count)
{
	for (uint32_t done = 0; done < count; done += ALU_CONSTANTS_A_PACKET) {
		const uint32_t n = count - done < ALU_CONSTANTS_A_PACKET ? count - done : ALU_CONSTANTS_A_PACKET;
		put_registers(index + done, NULL, n);
	}
}

static void put_im_load(const XsdkShader *shader, bool size_in_bytes)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_IM_LOAD, 2);
	packet[1] = shader->address | shader->type;
	packet[2] = size_in_bytes ? shader->dword_count * 4 : shader->dword_count;
}

// Xe_pInvalidateGpuCache, as Xe_VB_Unlock runs it over the vertices (xe.c:732-740, 752-762).
static void invalidate_vertices(uint32_t address, uint32_t size_bytes)
{
	const uint32_t range[] = { size_bytes, address };
	put_register(COHER_STATUS_HOST, COHER_VERTEX_CACHE);
	put_registers(COHER_SIZE_HOST, range, 2);
	volatile uint32_t *const packet = xsdk_gpu_reserve(6);
	packet[0] = xsdk_gpu_type3(PM4_WAIT_REG_MEM, 5);
	packet[1] = 3;
	packet[2] = COHER_STATUS_HOST;
	packet[3] = 0;
	packet[4] = COHER_BUSY;
	packet[5] = 8;
}

// libxenon's frame: Xe_pStuff, then Xe_pSetState with everything dirty, then Xe_DrawPrimitive (xe.c:1688-1744,
// 1943-1957).
static void draw_as_libxenon(
	const Variant *variant, const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices)
{
	const uint32_t control[] = { variant->depth_test ? DEPTH_LEQUAL_WRITE : 0, BLEND_ONE_ZERO, COLOR_CONTROL, 0,
		CLIP_D3D_SPACE, SC_MODE_WINDOW_OFFSET_NO_CULL, VTE_VIEWPORT_AND_W0, 0, EDRAM_MODE_COLOR_DEPTH };
	const uint32_t program[] = { vertex->program_control | pixel->program_control,
		vertex->context_misc | pixel->context_misc, INTERPOLATE_ALL_AT_CENTERS };
	const uint32_t flush_fetch[] = { 0, FLUSH_FETCH_1_VALUE, 0 };
	const uint32_t window[] = { 0, 0, WIDTH | HEIGHT << 16 };
	const uint32_t surface[] = { SURFACE_INFO_640, COLOR_INFO_BASE_240, 0 };
	const uint32_t indices[] = { MAX_VERTEX_INDEX, 0 };
	const uint32_t viewport[] = { STENCIL_MASKS, STENCIL_MASKS, 0, float_bits(WIDTH / 2.0f), float_bits(WIDTH / 2.0f),
		float_bits(-(float)HEIGHT / 2.0f), float_bits(HEIGHT / 2.0f), float_bits(1.0f), 0 };
	uint32_t fetch[FETCH_CONSTANT_DWORDS] = { 0 };
	fetch[VERTEX_FETCH_CONSTANT * 2] = vertices | VERTEX_FETCH_TYPE;
	fetch[VERTEX_FETCH_CONSTANT * 2 + 1] = VERTEX_FETCH_8IN32 | (uint32_t)sizeof triangle << 2;
	put_zeros(PA_SU_POLY_OFFSET_FRONT_SCALE, 8);
	put_registers(RB_DEPTHCONTROL, control, 9);
	put_im_load(pixel, variant->size_in_bytes);
	put_im_load(vertex, variant->size_in_bytes);
	put_registers(SQ_PROGRAM_CNTL, program, 3);
	if (variant->zero_alu)
		put_zeros(SHADER_CONSTANT_000_X, ALU_CONSTANT_DWORDS);
	put_registers(SHADER_CONSTANT_FETCH_00_0, fetch, FETCH_CONSTANT_DWORDS);
	put_registers(SHADER_CONSTANT_FLUSH_FETCH_0, flush_fetch, 3);
	put_zeros(PA_CL_UCP_0_X, CLIP_PLANE_DWORDS);
	if (variant->zero_bool_and_loop)
		put_zeros(SHADER_CONSTANT_BOOL_000_031, BOOL_AND_LOOP_DWORDS);
	put_registers(PA_SC_WINDOW_OFFSET, window, 3);
	put_registers(RB_SURFACE_INFO, surface, 3);
	put_register(COHER_DEST_BASE_7, 0);
	put_registers(VGT_MAX_VTX_INDX, indices, 2);
	put_register(RB_COLOR_MASK, 0xF);
	put_registers(RB_STENCILREFMASK_BF, viewport, 9);
	put_register(PA_SC_AA_CONFIG, 0);
	put_register(PA_SC_AA_MASK, 0xFFFF);
	put_register(COHER_DEST_BASE_1, 0);
	put_register(VGT_INDX_OFFSET, 0);
	if (!variant->predicated_draw_indx) {
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
		return;
	}
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_DRAW_INDX, 2) | PREDICATED;
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

static void draw_variant(const Variant *variant, const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *buffer, uint32_t scratch, bool has_display)
{
	for (uint32_t i = 0; i < PIXELS; i++)
		buffer[i] = MAGENTA;
	__asm__ volatile("sync" ::: "memory");
	const uint32_t address = xsdk_gpu_address(buffer);
	// A resolve and a fence first, so eDRAM holds the clear colour and far depth, and the GPU is idle.
	xsdk_gpu_resolve(scratch, WIDTH, HEIGHT, CLEAR_ARGB);
	invalidate_vertices(vertices, sizeof triangle);
	const NTSTATUS cleared = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (variant->sdk) {
		xsdk_gpu_set_target(WIDTH, HEIGHT);
		xsdk_gpu_set_shaders(vertex, pixel);
		xsdk_gpu_set_vertices(vertices, sizeof triangle);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
	} else {
		draw_as_libxenon(variant, vertex, pixel, vertices);
	}
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

static volatile uint32_t *upload_triangle(void)
{
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof triangle);
	if (!vertices)
		return NULL;
	uint32_t words[sizeof triangle / 4];
	memcpy(words, triangle, sizeof triangle);
	for (uint32_t i = 0; i < sizeof triangle / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	return vertices;
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
	volatile uint32_t *const vertices = upload_triangle();
	volatile uint32_t *const buffer = xsdk_gpu_alloc(PIXELS * 4);
	volatile uint32_t *const scratch = xsdk_gpu_alloc(PIXELS * 4);
	if (!NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, sizeof layout / sizeof layout[0], STRIDE_BYTES)) ||
		!vertices || !buffer || !scratch)
		return 1;
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		draw_variant(
			&variants[i], &vertex, &pixel, xsdk_gpu_address(vertices), buffer, xsdk_gpu_address(scratch), has_display);
	xsdk_display_close();
	xsdk_log_write("triangle: done; the GPU goes back at exit");
	return 0;
}
