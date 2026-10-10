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

// Draws milestone 6's triangle with each way of loading its shaders, logging how many pixels each draw covered.

enum {
	WIDTH = 640,
	HEIGHT = 480,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_MS = 1000,
	HOLD_MS = 3000,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	SLOT_DWORDS = 3,
	FIRST_FETCH_SLOT = 3,
	// xenia:src/xenia/gpu/xenos.h:1572-1622, register_table.inc
	PM4_IM_LOAD = 0x27,
	PM4_IM_LOAD_IMMEDIATE = 0x2B,
	SQ_PROGRAM_CNTL = 0x2180,
	INTERPOLATE_ALL_AT_CENTERS = 0xFFFFFFFF,
};

typedef enum {
	LOAD_SIZE_IN_DWORDS,
	LOAD_SIZE_IN_BYTES,
	LOAD_IMMEDIATE,
} Load;

typedef struct {
	const char *name;
	Load load;
	bool fence_before_resolve;
} Variant;

// The SDK's way first, then libxenon's byte count (libxenon:drivers/xenos/xe.c:1210-1215, 1383), then the inline load
// the resolve already uses on the console.
static const Variant variants[] = {
	{ "IM_LOAD, size in dwords", LOAD_SIZE_IN_DWORDS, false },
	{ "IM_LOAD, size in bytes", LOAD_SIZE_IN_BYTES, false },
	{ "IM_LOAD, size in bytes, a fence before the resolve", LOAD_SIZE_IN_BYTES, true },
	{ "IM_LOAD_IMMEDIATE, a fence before the resolve", LOAD_IMMEDIATE, true },
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

static void put_shader(const XsdkShader *shader, Load load)
{
	if (load == LOAD_IMMEDIATE) {
		volatile uint32_t *const packet = xsdk_gpu_reserve(3 + shader->dword_count);
		packet[0] = xsdk_gpu_type3(PM4_IM_LOAD_IMMEDIATE, 2 + shader->dword_count);
		packet[1] = shader->type;
		packet[2] = shader->dword_count;
		for (uint32_t i = 0; i < shader->dword_count; i++)
			packet[3 + i] = shader->microcode[i];
		return;
	}
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_IM_LOAD, 2);
	packet[1] = shader->address | shader->type;
	packet[2] = load == LOAD_SIZE_IN_BYTES ? shader->dword_count * 4 : shader->dword_count;
}

// As xsdk_gpu_set_shaders does, but with the variant's load.
static void load_shaders(const XsdkShader *vertex, const XsdkShader *pixel, Load load)
{
	put_shader(pixel, load);
	put_shader(vertex, load);
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
	packet[0] = 2 << 16 | SQ_PROGRAM_CNTL;
	packet[1] = vertex->program_control | pixel->program_control;
	packet[2] = vertex->context_misc | pixel->context_misc;
	packet[3] = INTERPOLATE_ALL_AT_CENTERS;
}

static uint32_t first_drawn(volatile const uint32_t *pixels, uint32_t *drawn)
{
	const uint32_t clear = __builtin_bswap32(CLEAR_ARGB);
	uint32_t first = clear;
	*drawn = 0;
	for (uint32_t i = 0; i < PIXELS; i++) {
		if (pixels[i] == clear)
			continue;
		if (!*drawn)
			first = pixels[i];
		(*drawn)++;
	}
	return first;
}

static void draw_variant(const Variant *variant, const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices,
	volatile uint32_t *buffer, uint32_t scratch, bool has_display)
{
	const uint32_t address = xsdk_gpu_address(buffer);
	// A resolve first, so eDRAM holds only the clear colour.
	xsdk_gpu_resolve(scratch, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	load_shaders(vertex, pixel, variant->load);
	xsdk_gpu_set_vertices(vertices, sizeof triangle);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, 3);
	NTSTATUS drawn_status = STATUS_SUCCESS;
	if (variant->fence_before_resolve)
		drawn_status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	uint32_t drawn = 0;
	const uint32_t first = first_drawn(buffer, &drawn);
	xsdk_log_write("triangle: %s: fences 0x%08X 0x%08X; drew %u pixels, the first %08X", variant->name,
		(unsigned)drawn_status, (unsigned)status, (unsigned)drawn, (unsigned)first);
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
	if (!NT_SUCCESS(xsdk_shader_load(&vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex, layout, sizeof layout / sizeof layout[0], STRIDE_BYTES)))
		return 1;
	for (uint32_t slot = FIRST_FETCH_SLOT; slot < FIRST_FETCH_SLOT + 2; slot++) {
		volatile const uint32_t *const fetch = &vertex.microcode[slot * SLOT_DWORDS];
		xsdk_log_write("triangle: patched fetch in slot %u: %08X %08X %08X", (unsigned)slot, (unsigned)fetch[0],
			(unsigned)fetch[1], (unsigned)fetch[2]);
	}
	volatile uint32_t *const vertices = upload_triangle();
	volatile uint32_t *const buffer = xsdk_gpu_alloc(PIXELS * 4);
	volatile uint32_t *const scratch = xsdk_gpu_alloc(PIXELS * 4);
	if (!vertices || !buffer || !scratch)
		return 1;
	for (uint32_t i = 0; i < sizeof variants / sizeof variants[0]; i++)
		draw_variant(
			&variants[i], &vertex, &pixel, xsdk_gpu_address(vertices), buffer, xsdk_gpu_address(scratch), has_display);
	xsdk_display_close();
	xsdk_log_write("triangle: done; the GPU goes back at exit");
	return 0;
}
