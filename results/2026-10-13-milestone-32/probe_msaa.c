#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// How multisampled draws and resolves take PA_SC_AA_CONFIG and the scan converter's MSAA_ENABLE, and where a pixel's
// samples sit: each case clears with a resolve, draws, resolves, and counts the edge pixels at each blend of the colours.

enum {
	WIDTH = 256,
	HEIGHT = 128,
	FENCE_TIMEOUT_MS = 1000,
	CLEAR_ARGB = 0xFF202020,
	SHAPE_ARGB = 0xFFE0A060,
	// A resolve that writes nothing leaves this.
	SENTINEL = 0xFF00FFFF,
	VERTEX_FLOATS = 8,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	COLOR_OFFSET_BYTES = 16,
	MAX_VERTICES = 6,
	// A channel this close to a blend counts as it.
	TOLERANCE = 1,
	MAX_SAMPLES = 4,
	NO_OVERRIDE = 0xFFFFFFFF,
	PA_SU_SC_MODE_CNTL = 0x2205,
	PA_SC_AA_CONFIG = 0x2301,
	// PA_SU_SC_MODE_CNTL's window offset and msaa_enable (xenia:src/xenia/gpu/registers.h:474-475).
	VTX_WINDOW_OFFSET_ENABLE = 1 << 16,
	MSAA_ENABLE = 1 << 15,
	SWEEP_ROW = 64,
	SWEEP_COLUMN = 128,
};

enum {
	SHAPE_TRIANGLE,
	// An edge rising one pixel over the target's width, through row 64, the shape below it.
	SHAPE_ACROSS,
	// An edge moving one pixel right over the target's height, through column 128, the shape right of it.
	SHAPE_DOWN,
	SHAPES,
};

// The patched resolve writes this into PA_SC_AA_CONFIG just before its draw, unless NO_OVERRIDE.
extern uint32_t probe_resolve_aa_config;

typedef struct {
	float x;
	float y;
} Point;

typedef struct {
	uint32_t vertex_count;
	Point points[MAX_VERTICES];
} Shape;

// In pixels, a pixel's square from (x, y) to (x + 1, y + 1).
static const Shape shapes[SHAPES] = {
	[SHAPE_TRIANGLE] = { 3, { { 16.3f, 12.7f }, { 240.1f, 40.4f }, { 40.6f, 116.2f } } },
	[SHAPE_ACROSS] = { 6, { { 0, 64 }, { 256, 65 }, { 0, 128 }, { 0, 128 }, { 256, 65 }, { 256, 128 } } },
	[SHAPE_DOWN] = { 6, { { 128, 0 }, { 256, 0 }, { 129, 128 }, { 129, 128 }, { 256, 0 }, { 256, 128 } } },
};

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_COLOR, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = COLOR_OFFSET_BYTES },
};

typedef struct {
	uint32_t samples;
	uint32_t draw_config;
	bool msaa_enable;
	uint32_t resolve_config;
} Case;

typedef struct {
	XsdkShader vertex;
	XsdkShader pixel;
	uint32_t vertices[SHAPES];
	volatile uint32_t *buffer;
	uint32_t scratch;
} Probe;

static uint32_t reference[HEIGHT][WIDTH];

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static volatile uint32_t *upload_shape(const Shape *shape)
{
	float vertices[MAX_VERTICES][VERTEX_FLOATS];
	const float colour[4] = { (SHAPE_ARGB >> 16 & 0xFF) / 255.0f, (SHAPE_ARGB >> 8 & 0xFF) / 255.0f,
		(SHAPE_ARGB & 0xFF) / 255.0f, 1.0f };
	for (uint32_t i = 0; i < shape->vertex_count; i++) {
		// Edges half a pixel early (xenia:src/xenia/gpu/draw_util.cc:837).
		vertices[i][0] = (shape->points[i].x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
		vertices[i][1] = 1.0f - (shape->points[i].y - 0.5f) / (HEIGHT / 2.0f);
		vertices[i][2] = 0.0f;
		vertices[i][3] = 1.0f;
		memcpy(&vertices[i][4], colour, sizeof colour);
	}
	volatile uint32_t *const memory = xsdk_gpu_alloc(sizeof vertices);
	if (!memory)
		return NULL;
	uint32_t words[sizeof vertices / 4];
	memcpy(words, vertices, sizeof vertices);
	for (uint32_t i = 0; i < sizeof vertices / 4; i++)
		memory[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	xsdk_gpu_invalidate(xsdk_gpu_address(memory), sizeof vertices);
	return memory;
}

static void set_target(const Case *c)
{
	xsdk_gpu_set_target_samples(WIDTH, HEIGHT, c->samples);
	if (c->draw_config != NO_OVERRIDE)
		put_register(PA_SC_AA_CONFIG, c->draw_config);
	put_register(PA_SU_SC_MODE_CNTL, VTX_WINDOW_OFFSET_ENABLE | (c->msaa_enable ? MSAA_ENABLE : 0));
}

static bool draw(const Probe *probe, const Case *c, uint32_t shape)
{
	for (uint32_t i = 0; i < WIDTH * HEIGHT; i++)
		probe->buffer[i] = SENTINEL;
	__asm__ volatile("sync" ::: "memory");
	probe_resolve_aa_config = c->resolve_config;
	set_target(c);
	xsdk_gpu_resolve(probe->scratch, WIDTH, HEIGHT, CLEAR_ARGB);
	set_target(c);
	xsdk_gpu_set_shaders(&probe->vertex, &probe->pixel);
	xsdk_gpu_set_vertices(0, probe->vertices[shape], MAX_VERTICES * STRIDE_BYTES);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, shapes[shape].vertex_count);
	xsdk_gpu_resolve(xsdk_gpu_address(probe->buffer), WIDTH, HEIGHT, CLEAR_ARGB);
	const NTSTATUS status = xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS);
	probe_resolve_aa_config = NO_OVERRIDE;
	if (!NT_SUCCESS(status))
		xsdk_log_write("msaa: the fence didn't land: 0x%08X", (unsigned)status);
	return NT_SUCCESS(status);
}

static uint32_t pixel_argb(const Probe *probe, uint32_t x, uint32_t y)
{
	return __builtin_bswap32(probe->buffer[tiled_index(x, y, WIDTH)]);
}

// Which k of samples the pixel blends to, k samples of the shape and the rest the clear colour, or -1 for none.
static int blend_level(uint32_t argb, uint32_t samples)
{
	for (uint32_t k = 0; k <= samples; k++) {
		bool matches = true;
		for (uint32_t shift = 0; shift <= 24; shift += 8) {
			const int clear = CLEAR_ARGB >> shift & 0xFF;
			const int shape = SHAPE_ARGB >> shift & 0xFF;
			const int want = clear + (int)k * (shape - clear) / (int)samples;
			const int got = argb >> shift & 0xFF;
			matches = matches && got - want <= (int)TOLERANCE && want - got <= (int)TOLERANCE;
		}
		if (matches)
			return (int)k;
	}
	return -1;
}

// The distance from (x, y) to the line through a and b.
static float line_distance(Point a, Point b, float x, float y)
{
	const float dx = b.x - a.x;
	const float dy = b.y - a.y;
	const float cross = (x - a.x) * dy - (y - a.y) * dx;
	return __builtin_fabsf(cross) / __builtin_sqrtf(dx * dx + dy * dy);
}

// Whether a pixel's centre is within a pixel of one of the triangle's edges.
static bool on_edge(uint32_t x, uint32_t y)
{
	const Point *const p = shapes[SHAPE_TRIANGLE].points;
	for (uint32_t i = 0; i < 3; i++) {
		if (line_distance(p[i], p[(i + 1) % 3], x + 0.5f, y + 0.5f) <= 1.0f)
			return true;
	}
	return false;
}

static const char *describe(const Case *c, char *text, uint32_t size)
{
	char resolve[24];
	if (c->resolve_config == NO_OVERRIDE)
		snprintf(resolve, sizeof resolve, "as xsdk sets it");
	else
		snprintf(resolve, sizeof resolve, "%08X", (unsigned)c->resolve_config);
	char drawn[24];
	if (c->draw_config == NO_OVERRIDE)
		snprintf(drawn, sizeof drawn, "as xsdk sets it");
	else
		snprintf(drawn, sizeof drawn, "%08X", (unsigned)c->draw_config);
	snprintf(text, size, "%ux, draw AA %s, MSAA_ENABLE %s, resolve AA %s", (unsigned)c->samples, drawn,
		c->msaa_enable ? "on" : "off", resolve);
	return text;
}

// Counts the triangle's edge pixels at each blend, and the pixels off its edges that differ from one sample's.
static void check_triangle(const Probe *probe, const Case *c, bool keep)
{
	uint32_t levels[MAX_SAMPLES + 1] = { 0 };
	uint32_t other = 0;
	uint32_t unwritten = 0;
	uint32_t differ = 0;
	uint32_t first_other = UINT32_MAX;
	uint32_t first_differ = UINT32_MAX;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t argb = pixel_argb(probe, x, y);
			if (keep)
				reference[y][x] = argb;
			unwritten += probe->buffer[tiled_index(x, y, WIDTH)] == SENTINEL;
			if (!on_edge(x, y)) {
				if (argb != reference[y][x] && differ++ == 0)
					first_differ = y << 16 | x;
				continue;
			}
			const int level = blend_level(argb, c->samples);
			if (level < 0) {
				if (other++ == 0)
					first_other = y << 16 | x;
				continue;
			}
			levels[level]++;
		}
	}
	char text[96];
	char counts[64];
	uint32_t length = 0;
	for (uint32_t k = 0; k <= c->samples; k++)
		length += snprintf(counts + length, sizeof counts - length, " %u/%u: %u", (unsigned)k, (unsigned)c->samples,
			(unsigned)levels[k]);
	xsdk_log_write("msaa: %s: edge pixels%s, others %u; off the edges %u differ from 1x; %u unwritten",
		describe(c, text, sizeof text), counts, (unsigned)other, (unsigned)differ, (unsigned)unwritten);
	if (other)
		xsdk_log_write("msaa:   first other at (%u, %u): %08X", (unsigned)(first_other & 0xFFFF),
			(unsigned)(first_other >> 16), (unsigned)pixel_argb(probe, first_other & 0xFFFF, first_other >> 16));
	if (differ)
		xsdk_log_write("msaa:   first differing at (%u, %u): %08X, 1x %08X", (unsigned)(first_differ & 0xFFFF),
			(unsigned)(first_differ >> 16), (unsigned)pixel_argb(probe, first_differ & 0xFFFF, first_differ >> 16),
			(unsigned)reference[first_differ >> 16][first_differ & 0xFFFF]);
}

// Logs where the blend changes along the swept row or column, as level@position.
static void log_sweep(const Probe *probe, const Case *c, uint32_t shape)
{
	const bool across = shape == SHAPE_ACROSS;
	const uint32_t count = across ? WIDTH : HEIGHT;
	char runs[160];
	uint32_t length = 0;
	int last = -2;
	for (uint32_t i = 0; i < count && length < sizeof runs - 16; i++) {
		const uint32_t argb = across ? pixel_argb(probe, i, SWEEP_ROW) : pixel_argb(probe, SWEEP_COLUMN, i);
		const int level = blend_level(argb, c->samples);
		if (level == last)
			continue;
		last = level;
		length += snprintf(runs + length, sizeof runs - length, " %d@%u", level, (unsigned)i);
	}
	char text[96];
	xsdk_log_write("msaa: %s: %s sweep:%s", describe(c, text, sizeof text), across ? "row 64" : "column 128", runs);
}

static void run_cases(const Probe *probe)
{
	const Case one = { 1, NO_OVERRIDE, false, NO_OVERRIDE };
	if (!draw(probe, &one, SHAPE_TRIANGLE))
		return;
	check_triangle(probe, &one, true);
	// libxenon's values (libxenon:drivers/xenos/xe.c:1738-1740), the samples less one as XAM left 4x
	// (docs/HARDWARE.md, "A resolve needs multisampling off"), and none.
	const uint32_t configs[2][3] = { { 0x8002, 0x8001, 0 }, { 0xC004, 0xC003, 0 } };
	for (uint32_t s = 0; s < 2; s++) {
		for (uint32_t i = 0; i < 3; i++) {
			for (uint32_t enable = 0; enable < 2; enable++) {
				for (uint32_t resolve = 0; resolve < 2; resolve++) {
					const uint32_t config = configs[s][i];
					const Case c = { s ? 4 : 2, config, enable == 0, resolve ? 0 : config };
					if (!draw(probe, &c, SHAPE_TRIANGLE))
						return;
					check_triangle(probe, &c, false);
				}
			}
		}
	}
	const Case sweeps[] = {
		one,
		{ 2, NO_OVERRIDE, true, NO_OVERRIDE },
		{ 2, 0x8001, true, 0x8001 },
		{ 4, NO_OVERRIDE, true, NO_OVERRIDE },
		{ 4, 0xC003, true, 0xC003 },
	};
	for (uint32_t i = 0; i < sizeof sweeps / sizeof sweeps[0]; i++) {
		for (uint32_t shape = SHAPE_ACROSS; shape <= SHAPE_DOWN; shape++) {
			if (!draw(probe, &sweeps[i], shape))
				return;
			log_sweep(probe, &sweeps[i], shape);
		}
	}
}

int main(void)
{
	xsdk_log_write("msaa probe");
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("msaa: xsdk_gpu_open returned 0x%08X", (unsigned)status);
		return 1;
	}
	Probe probe = { .vertex = { .microcode = NULL }, .pixel = { .microcode = NULL } };
	if (!NT_SUCCESS(xsdk_shader_load(&probe.vertex, triangle_vertex_shader, sizeof triangle_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&probe.pixel, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(
			&probe.vertex, layout, sizeof layout / sizeof layout[0], (const uint32_t[]){ STRIDE_BYTES }))) {
		xsdk_log_write("msaa: the shaders didn't load");
		return 1;
	}
	for (uint32_t i = 0; i < SHAPES; i++) {
		volatile uint32_t *const vertices = upload_shape(&shapes[i]);
		if (!vertices)
			return 1;
		probe.vertices[i] = xsdk_gpu_address(vertices);
	}
	probe.buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	volatile uint32_t *const scratch = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!probe.buffer || !scratch)
		return 1;
	probe.scratch = xsdk_gpu_address(scratch);
	run_cases(&probe);
	xsdk_log_write("msaa: done");
	return xsdk_gpu_close() == STATUS_SUCCESS ? 0 : 1;
}
