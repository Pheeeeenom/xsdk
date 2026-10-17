#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "probe_gpu.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// Which shapes of constant reads at a run-time index, c[k+a0], the GPU reads otherwise than Xenia's model (milestone
// 45, step 4): each case's vertex shader exports what it read over four columns of the target, each column's vertices
// with an a0 of their own, and each column's centre pixel names the constant the GPU read. Then a stress mode, for the
// reads xsdk_test still finds wrong in some frames: one draw of a grid of small quads, each with its own a0, under each
// load, from none to a pixel side busy enough that vertex threads wait at their allocs.

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

typedef struct {
	const char *name;
	const uint32_t *container;
	uint32_t bytes;
	// The row k the case reads at, c[k+a0].
	uint32_t row;
} StressCase;

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
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	INDEX_OFFSET_BYTES = 16,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	// c[n]'s z holds n ^ CHECK (tools/gen_relative_constant_probe.py).
	CHECK = 0x5A,
	CELL_TEXT_BYTES = 16,
	CASE_COUNT = sizeof probe_cases / sizeof probe_cases[0],
	// The stress grid: 64 by 36 cells of 20 by 20 pixels, each checked at its centre.
	GRID_CELL_PIXELS = 20,
	GRID_COLUMNS = WIDTH / GRID_CELL_PIXELS,
	GRID_ROWS = HEIGHT / GRID_CELL_PIXELS,
	GRID_CELLS = GRID_COLUMNS * GRID_ROWS,
	GRID_VERTICES = GRID_CELLS * QUAD_VERTICES,
	STRESS_DRAWS = 20,
	WRONG_DRAWS_LOGGED = 3,
	WRONG_CELLS_LOGGED = 4,
	WRONG_TEXT_BYTES = 64,
	STRESS_CASE_COUNT = sizeof stress_cases / sizeof stress_cases[0],
};

// A draw's vertex count is 16 bits (xenia:src/xenia/gpu/registers.h:327, VGT_DRAW_INITIATOR's num_indices).
_Static_assert(GRID_VERTICES <= 0xFFFF, "the grid in one draw");

// How busy the pixel side is: the grid's pixel shader, and how many full-screen draws of it go before the grid.
typedef struct {
	const char *name;
	bool busy;
	uint32_t passes;
} Load;

static const Load loads[] = {
	{ "light", false, 0 },
	{ "busy pixels", true, 0 },
	{ "busy pixels, 4 passes", true, 4 },
};

enum {
	LOAD_COUNT = sizeof loads / sizeof loads[0],
};

typedef struct {
	uint32_t draws_wrong;
	uint32_t cells_wrong;
} StressRun;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = INDEX_OFFSET_BYTES },
};

static const uint32_t strides[] = { STRIDE_BYTES };

// The unit square's two triangles, each corner its left or right and bottom or top.
static const uint8_t corners[QUAD_VERTICES][2] = {
	{ 0, 0 },
	{ 1, 0 },
	{ 0, 1 },
	{ 0, 1 },
	{ 1, 0 },
	{ 1, 1 },
};

typedef struct {
	uint32_t passed;
	uint32_t differed;
} Totals;

static float constants[XSDK_FLOAT_CONSTANTS][4];
static volatile uint32_t *vertices[MODES];
static volatile uint32_t *grid;
static volatile uint32_t *screen;
static volatile uint32_t *image;
static XsdkShader pixel;
static XsdkShader busy_pixel;

// Quad number quad from x's left and right and y's bottom and top in clip space, its index vector holding a0.
static void put_quad(volatile uint32_t *memory, uint32_t quad, const float x[2], const float y[2], float a0)
{
	for (uint32_t v = 0; v < QUAD_VERTICES; v++) {
		const float vertex[VERTEX_FLOATS] = { x[corners[v][0]], y[corners[v][1]], 0.5f, 1.0f, a0, probe_index_rest[0],
			probe_index_rest[1], probe_index_rest[2] };
		uint32_t words[VERTEX_FLOATS];
		memcpy(words, vertex, sizeof words);
		for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
			memory[(quad * QUAD_VERTICES + v) * VERTEX_FLOATS + i] = words[i];
	}
}

static void hand_to_gpu(volatile uint32_t *memory, uint32_t size_bytes)
{
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), size_bytes);
}

// A quad over each column, its index vector holding the column's a0.
static volatile uint32_t *make_vertices(const float *cell_index)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(DRAW_VERTICES * STRIDE_BYTES);
	if (!memory)
		return NULL;
	const float y[2] = { -1.0f, 1.0f };
	for (uint32_t cell = 0; cell < CELLS; cell++) {
		const float x[2] = { -1.0f + cell * 2.0f / CELLS, -1.0f + (cell + 1) * 2.0f / CELLS };
		put_quad(memory, cell, x, y, cell_index[cell]);
	}
	hand_to_gpu(memory, DRAW_VERTICES * STRIDE_BYTES);
	return memory;
}

static uint32_t grid_a0(uint32_t cell)
{
	return PROBE_A0_FIRST + cell % PROBE_A0_COUNT;
}

// The cells row by row from the top left, cell i's index vector holding grid_a0(i).
static volatile uint32_t *make_grid(uint32_t columns, uint32_t rows)
{
	volatile uint32_t *const memory = xsdk_gpu_alloc(columns * rows * QUAD_BYTES);
	if (!memory)
		return NULL;
	for (uint32_t cell = 0; cell < columns * rows; cell++) {
		const uint32_t column = cell % columns;
		const uint32_t row = cell / columns;
		const float x[2] = { -1.0f + column * 2.0f / columns, -1.0f + (column + 1) * 2.0f / columns };
		const float y[2] = { 1.0f - (row + 1) * 2.0f / rows, 1.0f - row * 2.0f / rows };
		put_quad(memory, cell, x, y, (float)grid_a0(cell));
	}
	hand_to_gpu(memory, columns * rows * QUAD_BYTES);
	return memory;
}

// c[n] holds m = n + shift, its low 8 bits: m, 255 - m and m ^ CHECK, each a quarter into its 8-bit step, and w 1, as
// the generator fills it with shift 0.
static void fill_constants(uint32_t shift)
{
	for (uint32_t n = 0; n < XSDK_FLOAT_CONSTANTS; n++) {
		const uint32_t m = (n + shift) & 0xFF;
		constants[n][0] = (m + 0.25f) / 255.0f;
		constants[n][1] = (255 - m + 0.25f) / 255.0f;
		constants[n][2] = ((m ^ CHECK) + 0.25f) / 255.0f;
		constants[n][3] = 1.0f;
	}
}

// The constant a pixel holds, under fill_constants(shift), or -1 for none whole; a resolve stores blue, green, red,
// alpha from the top byte, as tests/probe_constants.c reads it.
static int constant_read(uint32_t stored, uint32_t shift)
{
	const uint32_t m = stored >> 8 & 0xFF;
	const bool whole = (stored >> 16 & 0xFF) == 255 - m && stored >> 24 == (m ^ CHECK) && (stored & 0xFF) == 0xFF;
	return whole ? (int)((m - shift) & 0xFF) : -1;
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
		const int reg = constant_read(stored, 0);
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

// False, logged, if the case's vertex shader didn't load or couldn't be laid out.
static bool load_case(XsdkShader *shader, const char *name, const uint32_t *container, uint32_t bytes)
{
	if (!NT_SUCCESS(xsdk_shader_load(shader, container, bytes))) {
		xsdk_log_write("relative constants probe: %s: its container didn't load; skipped", name);
		return false;
	}
	if (!NT_SUCCESS(xsdk_shader_set_vertex_layout(shader, layout, sizeof layout / sizeof layout[0], strides))) {
		xsdk_log_write("relative constants probe: %s: its fetches couldn't be laid out; skipped", name);
		xsdk_shader_free(shader);
		return false;
	}
	return true;
}

// False if a fence didn't land, after which the GPU is wedged and the shader stays loaded.
static bool run_case(const ProbeCase *c, Totals *t)
{
	XsdkShader shader;
	if (!load_case(&shader, c->name, c->container, c->bytes))
		return true;
	for (uint32_t mode = 0; mode < MODES; mode++) {
		if (!run_mode(c, &shader, mode, t))
			return false;
	}
	xsdk_shader_free(&shader);
	return true;
}

// How many of the grid's cells didn't read c[row + a0] under fill_constants(shift); the first few go into text.
static uint32_t grid_wrong(const StressCase *c, uint32_t shift, char *text, size_t size)
{
	uint32_t wrong = 0;
	for (uint32_t cell = 0; cell < GRID_CELLS; cell++) {
		const uint32_t column = cell % GRID_COLUMNS;
		const uint32_t row = cell / GRID_COLUMNS;
		const uint32_t stored = image[tiled_index(
			column * GRID_CELL_PIXELS + GRID_CELL_PIXELS / 2, row * GRID_CELL_PIXELS + GRID_CELL_PIXELS / 2, WIDTH)];
		const int want = (int)(c->row + grid_a0(cell));
		const int reg = constant_read(stored, shift);
		if (reg == want)
			continue;
		if (wrong++ >= WRONG_CELLS_LOGGED)
			continue;
		size_t used = strlen(text);
		snprintf(text + used, size - used, " (%u,%u) wants c%d, read", (unsigned)column, (unsigned)row, want);
		append_register(text, size, reg);
		used = strlen(text);
		snprintf(text + used, size - used, " %08X;", (unsigned)stored);
	}
	return wrong;
}

// One draw of the grid after the load's passes; counts its wrong cells and logs the first few of the first wrong
// draws. False if the fence didn't land.
static bool stress_draw(const StressCase *c, const XsdkShader *shader, uint32_t load, uint32_t draw, StressRun *run)
{
	const Load *const l = &loads[load];
	// Each draw's constants differ, so a register an earlier draw left reads as another constant.
	const uint32_t shift = 1 + load * STRESS_DRAWS + draw;
	fill_constants(shift);
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertex_constants(0, constants[0], XSDK_FLOAT_CONSTANTS);
	xsdk_gpu_set_shaders(shader, l->busy ? &busy_pixel : &pixel);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(screen), QUAD_BYTES);
	for (uint32_t i = 0; i < l->passes; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	xsdk_gpu_set_vertices(0, xsdk_gpu_address(grid), GRID_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, GRID_VERTICES);
	xsdk_gpu_resolve(xsdk_gpu_address(image), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("relative constants probe: stress %s [%s] draw %u: the fence didn't land: 0x%08X", c->name,
			l->name, (unsigned)draw, (unsigned)status);
		probe_report_cp("relative constants probe", "after the hang");
		return false;
	}
	char text[WRONG_CELLS_LOGGED * WRONG_TEXT_BYTES] = "";
	const uint32_t wrong = grid_wrong(c, shift, text, sizeof text);
	if (wrong && run->draws_wrong < WRONG_DRAWS_LOGGED) {
		xsdk_log_write("relative constants probe: stress %s [%s] draw %u: %u of %u cells wrong:%s", c->name, l->name,
			(unsigned)draw, (unsigned)wrong, (unsigned)GRID_CELLS, text);
	}
	run->draws_wrong += wrong != 0;
	run->cells_wrong += wrong;
	return true;
}

// The case's draws under one load; false if a fence didn't land.
static bool run_stress_load(const StressCase *c, const XsdkShader *shader, uint32_t load, Totals *t)
{
	struct timespec start;
	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	StressRun run = { 0 };
	for (uint32_t draw = 0; draw < STRESS_DRAWS; draw++) {
		if (!stress_draw(c, shader, load, draw, &run))
			return false;
	}
	xsdk_log_write("relative constants probe: stress %s [%s]: %u of %u draws wrong, %u of %u cells in all, %s, %u ms",
		c->name, loads[load].name, (unsigned)run.draws_wrong, (unsigned)STRESS_DRAWS, (unsigned)run.cells_wrong,
		(unsigned)(STRESS_DRAWS * GRID_CELLS), run.draws_wrong ? "DIFFERS" : "PASS",
		(unsigned)probe_elapsed_ms(&start));
	t->passed += run.draws_wrong == 0;
	t->differed += run.draws_wrong != 0;
	return true;
}

// False if a fence didn't land, after which the GPU is wedged and the shader stays loaded.
static bool run_stress_case(const StressCase *c, Totals *t)
{
	XsdkShader shader;
	if (!load_case(&shader, c->name, c->container, c->bytes))
		return true;
	for (uint32_t load = 0; load < LOAD_COUNT; load++) {
		if (!run_stress_load(c, &shader, load, t))
			return false;
	}
	xsdk_shader_free(&shader);
	return true;
}

static void log_stress_setup(void)
{
	xsdk_log_write(
		"relative constants probe: stress: %u cases, each one draw of %u by %u quads of %u pixels square, %u "
		"vertices, cell i's a0 %u + i %% %u, row by row from the top left, its centre pixel checked; %u draws "
		"a load, each with c[n] holding n plus a shift of its own",
		(unsigned)STRESS_CASE_COUNT, (unsigned)GRID_COLUMNS, (unsigned)GRID_ROWS, (unsigned)GRID_CELL_PIXELS,
		(unsigned)GRID_VERTICES, (unsigned)PROBE_A0_FIRST, (unsigned)PROBE_A0_COUNT, (unsigned)STRESS_DRAWS);
	for (uint32_t load = 0; load < LOAD_COUNT; load++) {
		xsdk_log_write(
			"relative constants probe: stress [%s]: the pixel shader's %u ALU instructions before its export, "
			"%u full-screen draws of the case before the grid",
			loads[load].name, loads[load].busy ? (unsigned)PROBE_BUSY_INSTRUCTIONS : 0u, (unsigned)loads[load].passes);
	}
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
	fill_constants(0);
	image = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	for (uint32_t mode = 0; mode < MODES; mode++)
		vertices[mode] = make_vertices(probe_cell_index[mode]);
	grid = make_grid(GRID_COLUMNS, GRID_ROWS);
	screen = make_grid(1, 1);
	if (!image || !vertices[0] || !vertices[1] || !grid || !screen ||
		!NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&busy_pixel, probe_busy_pixel_shader, sizeof probe_busy_pixel_shader))) {
		xsdk_log_write("relative constants probe: FAILED, the buffers or the pixel shaders couldn't be made");
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
	log_stress_setup();
	Totals stress = { 0 };
	for (uint32_t i = 0; i < STRESS_CASE_COUNT; i++) {
		if (!run_stress_case(&stress_cases[i], &stress)) {
			xsdk_log_write(
				"relative constants probe: FAILED, the GPU didn't finish stress %s; %u PASS, %u DIFFERS "
				"before it",
				stress_cases[i].name, (unsigned)stress.passed, (unsigned)stress.differed);
			return 1;
		}
	}
	status = xsdk_gpu_close();
	xsdk_log_write(
		"relative constants probe: done, %u draws: %u PASS, %u DIFFERS; stress, %u runs of %u draws: %u "
		"PASS, %u DIFFERS; xsdk_gpu_close 0x%08X",
		(unsigned)(t.passed + t.differed), (unsigned)t.passed, (unsigned)t.differed,
		(unsigned)(stress.passed + stress.differed), (unsigned)STRESS_DRAWS, (unsigned)stress.passed,
		(unsigned)stress.differed, (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
