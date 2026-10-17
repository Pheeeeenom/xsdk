#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Which shapes of constant reads at a run-time index, c[k+a0], the GPU reads otherwise than Xenia's model (milestone
// 45, step 4): each case's vertex shader exports what it read over four columns of the target, each column's vertices
// with an a0 of their own, and each column's centre pixel names the constant the GPU read.

enum {
	MODES = 2,
	CELLS = 4,
};

typedef struct {
	const char *name;
	const uint32_t *container;
	uint32_t bytes;
	// The constant Xenia's model reads in each column, for each mode's a0.
	uint8_t expected[MODES][CELLS];
} ProbeCase;

#include "probe_relative_constant_cases.h"

_Static_assert(sizeof probe_mode_names / sizeof probe_mode_names[0] == MODES, "the generator's modes");

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	CELL_WIDTH = WIDTH / CELLS,
	QUAD_VERTICES = 6,
	DRAW_VERTICES = CELLS * QUAD_VERTICES,
	// A position, then the index vector, a0 in its x.
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	INDEX_OFFSET_BYTES = 16,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	// c[n]'s z holds n ^ CHECK (tools/gen_relative_constant_probe.py).
	CHECK = 0x5A,
	CELL_TEXT_BYTES = 16,
	CASE_COUNT = sizeof probe_cases / sizeof probe_cases[0],
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = INDEX_OFFSET_BYTES },
};

static const uint32_t strides[] = { STRIDE_BYTES };

// The unit square's two triangles.
static const float corners[QUAD_VERTICES][2] = {
	{ 0.0f, 0.0f },
	{ 1.0f, 0.0f },
	{ 0.0f, 1.0f },
	{ 0.0f, 1.0f },
	{ 1.0f, 0.0f },
	{ 1.0f, 1.0f },
};

typedef struct {
	uint32_t passed;
	uint32_t differed;
} Totals;

static float constants[XSDK_FLOAT_CONSTANTS][4];
static volatile uint32_t *vertices[MODES];
static volatile uint32_t *image;
static XsdkShader pixel;

// A quad over each column, its index vector holding the column's a0.
static volatile uint32_t *make_vertices(const float *cell_index)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(DRAW_VERTICES * STRIDE_BYTES);
	if (!memory)
		return NULL;
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
			const float vertex[VERTEX_FLOATS] = { -1.0f + (cell + corners[v][0]) * 2.0f / CELLS,
				-1.0f + 2.0f * corners[v][1], 0.5f, 1.0f, cell_index[cell], probe_index_rest[0], probe_index_rest[1],
				probe_index_rest[2] };
			uint32_t words[VERTEX_FLOATS];
			memcpy(words, vertex, sizeof words);
			for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
				memory[(cell * QUAD_VERTICES + v) * VERTEX_FLOATS + i] = words[i];
		}
	}
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), DRAW_VERTICES * STRIDE_BYTES);
	return memory;
}

// c[n] holds n, 255 - n and n ^ CHECK, each a quarter into its 8-bit step, and w 1, as the generator fills it.
static void fill_constants(void)
{
	for (uint32_t n = 0; n < XSDK_FLOAT_CONSTANTS; n++) {
		constants[n][0] = (n + 0.25f) / 255.0f;
		constants[n][1] = (255 - n + 0.25f) / 255.0f;
		constants[n][2] = ((n ^ CHECK) + 0.25f) / 255.0f;
		constants[n][3] = 1.0f;
	}
}

// The constant a pixel holds, or -1 for none whole; a resolve stores blue, green, red, alpha from the top byte, as
// tests/probe_constants.c reads it.
static int constant_read(uint32_t stored)
{
	const uint32_t n = stored >> 8 & 0xFF;
	const bool whole = (stored >> 16 & 0xFF) == 255 - n && stored >> 24 == (n ^ CHECK) && (stored & 0xFF) == 0xFF;
	return whole ? (int)n : -1;
}

static void append_register(char *text, size_t size, int reg)
{
	const size_t used = strlen(text);
	if (reg < 0)
		snprintf(text + used, size - used, " c?");
	else
		snprintf(text + used, size - used, " c%d", reg);
}

// Draws the case with the mode's a0 and logs what each column read; false if the fence didn't land.
static bool run_mode(const ProbeCase *c, const XsdkShader *shader, uint32_t mode, Totals *t)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertex_constants(0, constants[0], XSDK_FLOAT_CONSTANTS);
	xsdk_gpu_set_shaders(shader, &pixel);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(vertices[mode]), DRAW_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, DRAW_VERTICES);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("relative constants probe: %s [%s]: the fence didn't land: 0x%08X", c->name,
			probe_mode_names[mode], (unsigned)status);
		probe_report_cp("relative constants probe", "after the hang");
		return false;
	}
	char model[CELLS * CELL_TEXT_BYTES] = "";
	char gpu[CELLS * CELL_TEXT_BYTES] = "";
	char pixels[CELLS * CELL_TEXT_BYTES] = "";
	bool same = true;
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		const uint32_t stored = image[tiled_index(cell * CELL_WIDTH + CELL_WIDTH / 2, HEIGHT / 2, WIDTH)];
		const int reg = constant_read(stored);
		same = same && reg == c->expected[mode][cell];
		append_register(model, sizeof model, c->expected[mode][cell]);
		append_register(gpu, sizeof gpu, reg);
		const size_t used = strlen(pixels);
		snprintf(pixels + used, sizeof pixels - used, " %08X", (unsigned)stored);
	}
	xsdk_log_write("relative constants probe: %s [%s]: model reads%s, GPU read%s, %s, pixels%s", c->name,
		probe_mode_names[mode], model, gpu, same ? "PASS" : "DIFFERS", pixels);
	t->passed += same;
	t->differed += !same;
	return true;
}

// False if a fence didn't land, after which the GPU is wedged and the shader stays loaded.
static bool run_case(const ProbeCase *c, Totals *t)
{
	XsdkShader shader;
	if (!NT_SUCCESS(xsdk_shader_load(&shader, c->container, c->bytes))) {
		xsdk_log_write("relative constants probe: %s: its container didn't load; skipped", c->name);
		return true;
	}
	if (!NT_SUCCESS(xsdk_shader_set_vertex_layout(&shader, layout, sizeof layout / sizeof layout[0], strides))) {
		xsdk_log_write("relative constants probe: %s: its fetches couldn't be laid out; skipped", c->name);
		xsdk_shader_free(&shader);
		return true;
	}
	for (uint32_t mode = 0; mode < MODES; mode++) {
		if (!run_mode(c, &shader, mode, t))
			return false;
	}
	xsdk_shader_free(&shader);
	return true;
}

int main(void)
{
	xsdk_log_write("relative constants probe: %u cases; c[n] draws as red n, green 255 - n, blue n ^ 0x%02X",
		(unsigned)CASE_COUNT, (unsigned)CHECK);
	for (uint32_t mode = 0; mode < MODES; mode++) {
		xsdk_log_write("relative constants probe: [%s]: the columns' a0 %d %d %d %d", probe_mode_names[mode],
			(int)probe_cell_index[mode][0], (int)probe_cell_index[mode][1], (int)probe_cell_index[mode][2],
			(int)probe_cell_index[mode][3]);
	}
	NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("relative constants probe: FAILED, xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	fill_constants();
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	for (uint32_t mode = 0; mode < MODES; mode++)
		vertices[mode] = make_vertices(probe_cell_index[mode]);
	if (!image || !vertices[0] || !vertices[1] ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader))) {
		xsdk_log_write("relative constants probe: FAILED, the buffers or the pixel shader couldn't be made");
		return 1;
	}
	Totals t = { 0 };
	for (uint32_t i = 0; i < CASE_COUNT; i++) {
		if (!run_case(&probe_cases[i], &t)) {
			xsdk_log_write("relative constants probe: FAILED, the GPU didn't finish %s; %u PASS, %u DIFFERS before it",
				probe_cases[i].name, (unsigned)t.passed, (unsigned)t.differed);
			return 1;
		}
	}
	status = xsdk_gpu_close();
	xsdk_log_write("relative constants probe: done, %u draws: %u PASS, %u DIFFERS; xsdk_gpu_close 0x%08X",
		(unsigned)(t.passed + t.differed), (unsigned)t.passed, (unsigned)t.differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
