#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Draws a triangle and resolves, then draws again without setting the target, which hung the GPU: an assert stops it.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	TRIANGLE_VERTICES = 3,
};

// Position in clip space, then colour.
static const float triangle[TRIANGLE_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.75f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f },
	{ -0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.75f, -0.75f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

static bool load_shaders(XsdkShader *vertex, XsdkShader *pixel)
{
	return NT_SUCCESS(xsdk_shader_load(vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
		NT_SUCCESS(xsdk_shader_set_vertex_layout(
			vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES })) &&
		NT_SUCCESS(xsdk_shader_load(pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
}

static void draw(const XsdkShader *vertex, const XsdkShader *pixel, uint32_t vertices)
{
	xsdk_gpu_set_shaders(vertex, pixel);
	xsdk_gpu_set_vertices(0, vertices, sizeof triangle);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, TRIANGLE_VERTICES);
}

int main(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("draw after resolve: xsdk_gpu_open 0x%08X", (unsigned)status);
	XsdkShader vertex = { .microcode = NULL };
	XsdkShader pixel = { .microcode = NULL };
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof triangle);
	volatile uint32_t *const buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!NT_SUCCESS(status) || !load_shaders(&vertex, &pixel) || !vertices || !buffer) {
		xsdk_log_write("draw after resolve: the set-up failed");
		return 1;
	}
	uint32_t words[sizeof triangle / 4];
	memcpy(words, triangle, sizeof triangle);
	for (uint32_t i = 0; i < sizeof triangle / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(vertices), sizeof triangle);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	draw(&vertex, &pixel, xsdk_gpu_address(vertices));
	xsdk_gpu_resolve(xsdk_gpu_address(buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_log_write("draw after resolve: the first frame's fence 0x%08X",
		(unsigned)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	xsdk_log_write("draw after resolve: drawing again without setting the target");
	draw(&vertex, &pixel, xsdk_gpu_address(vertices));
	xsdk_log_write("draw after resolve: the draw went through; its fence 0x%08X",
		(unsigned)xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
	return 1;
}
