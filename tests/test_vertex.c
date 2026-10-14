#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "tests.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/display.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/thread.h"

// One cell per vertex format, positions and colours from separate streams with their own strides, checked by pixel.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	BUFFERS = 2,
	FRAMES = 60,
	FRAME_MS = 16,
	FENCE_TIMEOUT_MS = 1000,
	MAX_MISSES_LOGGED = 8,
	CLEAR_ARGB = 0xFF202020,
	CELL_SIZE = 64,
	// Each cell is 2 by 2 squares of one colour each.
	SQUARES = 4,
	SQUARE_SIZE = CELL_SIZE / 2,
	VERTICES = SQUARES * 6,
	COMPONENTS = 4,
	// Fixed point and floats convert once more on the way to the 8-bit target.
	TOLERANCE = 1,
	MAX_CELLS = 24,
	POINTS_PER_CELL = SQUARES + 1,
	CELL_STEP = 130,
	ROW_STEP = 140,
	COLOUR_FORMATS_PER_ROW = 9,
};

typedef struct {
	const char *name;
	uint32_t position_format;
	uint32_t colour_format;
	uint32_t position_stream;
	uint32_t colour_stream;
	uint32_t position_stride;
	uint32_t colour_stride;
	uint32_t position_offset;
	uint32_t colour_offset;
	uint32_t x;
	uint32_t y;
} Cell;

typedef struct {
	const char *name;
	uint32_t bytes;
	uint32_t components;
	// Bits in each integer component; 0 for floats.
	uint32_t bits;
	bool is_signed;
	bool normalized;
	bool half;
} FormatSpec;

static const FormatSpec specs[XSDK_VERTEX_FORMATS] = {
	[XSDK_VERTEX_FLOAT1] = { "float1", 4, 1, 0, true, false, false },
	[XSDK_VERTEX_FLOAT2] = { "float2", 8, 2, 0, true, false, false },
	[XSDK_VERTEX_FLOAT3] = { "float3", 12, 3, 0, true, false, false },
	[XSDK_VERTEX_FLOAT4] = { "float4", 16, 4, 0, true, false, false },
	[XSDK_VERTEX_HALF2] = { "half2", 4, 2, 0, true, false, true },
	[XSDK_VERTEX_HALF4] = { "half4", 8, 4, 0, true, false, true },
	[XSDK_VERTEX_UBYTE4N] = { "ubyte4n", 4, 4, 8, false, true, false },
	[XSDK_VERTEX_BYTE4N] = { "byte4n", 4, 4, 8, true, true, false },
	[XSDK_VERTEX_UBYTE4] = { "ubyte4", 4, 4, 8, false, false, false },
	[XSDK_VERTEX_BYTE4] = { "byte4", 4, 4, 8, true, false, false },
	[XSDK_VERTEX_USHORT2N] = { "ushort2n", 4, 2, 16, false, true, false },
	[XSDK_VERTEX_SHORT2N] = { "short2n", 4, 2, 16, true, true, false },
	[XSDK_VERTEX_USHORT2] = { "ushort2", 4, 2, 16, false, false, false },
	[XSDK_VERTEX_SHORT2] = { "short2", 4, 2, 16, true, false, false },
	[XSDK_VERTEX_USHORT4N] = { "ushort4n", 8, 4, 16, false, true, false },
	[XSDK_VERTEX_SHORT4N] = { "short4n", 8, 4, 16, true, true, false },
	[XSDK_VERTEX_USHORT4] = { "ushort4", 8, 4, 16, false, false, false },
	[XSDK_VERTEX_SHORT4] = { "short4", 8, 4, 16, true, false, false },
};

// Colours a half float holds exactly, none at 0.5, which the 8-bit target would round either way.
static const float palette[SQUARES][COMPONENTS] = {
	{ 1.0f, 0.25f, 0.0f, 1.0f },
	{ 0.0f, 0.875f, 0.375f, 1.0f },
	{ 0.125f, 0.0f, 1.0f, 0.75f },
	{ 0.75f, 0.625f, 0.25f, 0.375f },
};

static bool is_integer(uint32_t format)
{
	return specs[format].bits && !specs[format].normalized;
}

// A square's colour: the palette, whole numbers for integer formats, a negative and an over-range value to show sign
// and scale.
static void colour_of(uint32_t cell, uint32_t format, uint32_t square, float out[COMPONENTS])
{
	const FormatSpec *const spec = &specs[format];
	for (uint32_t c = 0; c < COMPONENTS; c++) {
		const float value = palette[(square + cell) % SQUARES][c];
		out[c] = is_integer(format) ? (value > 0.5f ? 1.0f : 0.0f) : value;
	}
	// In blue, or red for formats without blue, so every signed format stores one.
	if (square == SQUARES - 1 && spec->is_signed)
		out[spec->components > 2 ? 2 : 0] = is_integer(format) ? -1.0f : -0.5f;
	if (square == SQUARES - 1 && is_integer(format))
		out[1] = 2.0f;
}

static int32_t round_to_int(float value)
{
	return (int32_t)(value < 0.0f ? value - 0.5f : value + 0.5f);
}

static uint32_t float_bits(float value)
{
	uint32_t bits;
	memcpy(&bits, &value, sizeof bits);
	return bits;
}

// The nearest half float; too small for a normal half is 0.
static uint16_t to_half(float value)
{
	const uint32_t bits = float_bits(value);
	const uint32_t sign = bits >> 16 & 0x8000;
	const int32_t exponent = (int32_t)(bits >> 23 & 0xFF) - 127 + 15;
	if (exponent <= 0)
		return (uint16_t)sign;
	const uint32_t rounded = ((uint32_t)exponent << 10 | (bits & 0x7FFFFF) >> 13) + (bits >> 12 & 1);
	return (uint16_t)(sign | rounded);
}

static float half_value(uint16_t bits)
{
	const uint32_t exponent = bits >> 10 & 0x1F;
	const float magnitude =
		exponent ? (float)(0x400 | (bits & 0x3FF)) * (float)(1u << exponent) / (float)(1u << 25) : 0.0f;
	return bits & 0x8000 ? -magnitude : magnitude;
}

// The stored integer for a component of a fixed-point format.
static uint32_t to_fixed(const FormatSpec *spec, float value)
{
	const float scale = spec->normalized ? (float)((1u << (spec->bits - spec->is_signed)) - 1) : 1.0f;
	return (uint32_t)round_to_int(value * scale) & ((1u << spec->bits) - 1);
}

// Components into 32-bit words, each component after the first in the bits above it.
static void encode(uint32_t format, const float value[COMPONENTS], uint32_t *words)
{
	const FormatSpec *const spec = &specs[format];
	memset(words, 0, spec->bytes);
	for (uint32_t c = 0; c < spec->components; c++) {
		if (!spec->bits && !spec->half) {
			words[c] = float_bits(value[c]);
			continue;
		}
		const uint32_t width = spec->half ? 16 : spec->bits;
		const uint32_t field = spec->half ? to_half(value[c]) : to_fixed(spec, value[c]);
		const uint32_t per_word = 32 / width;
		words[c / per_word] |= field << (c % per_word * width);
	}
}

// What the GPU should make of the words: each component as a float, missing ones 0 and w 1.
static void decode(uint32_t format, const uint32_t *words, float out[COMPONENTS])
{
	const FormatSpec *const spec = &specs[format];
	for (uint32_t c = 0; c < COMPONENTS; c++)
		out[c] = c == COMPONENTS - 1 ? 1.0f : 0.0f;
	for (uint32_t c = 0; c < spec->components; c++) {
		if (!spec->bits && !spec->half) {
			memcpy(&out[c], &words[c], sizeof out[c]);
			continue;
		}
		const uint32_t width = spec->half ? 16 : spec->bits;
		const uint32_t per_word = 32 / width;
		const uint32_t field = words[c / per_word] >> (c % per_word * width) & ((1u << width) - 1);
		if (spec->half) {
			out[c] = half_value((uint16_t)field);
			continue;
		}
		const int32_t number =
			spec->is_signed && field >> (width - 1) ? (int32_t)field - (int32_t)(1u << width) : (int32_t)field;
		const float scale = spec->normalized ? (float)((1u << (width - spec->is_signed)) - 1) : 1.0f;
		const float scaled = (float)number / scale;
		out[c] = spec->normalized && scaled < -1.0f ? -1.0f : scaled;
	}
}

static uint32_t to_byte(float value)
{
	const float rounded = value * 255.0f + 0.5f;
	return rounded < 0.0f ? 0 : rounded >= 255.0f ? 255 : (uint32_t)rounded;
}

// The ARGB a square should draw, from the colour the GPU decodes.
static uint32_t square_argb(const Cell *cell, uint32_t index, uint32_t square)
{
	float colour[COMPONENTS];
	uint32_t words[COMPONENTS];
	colour_of(index, cell->colour_format, square, colour);
	encode(cell->colour_format, colour, words);
	float decoded[COMPONENTS];
	decode(cell->colour_format, words, decoded);
	return to_byte(decoded[3]) << 24 | to_byte(decoded[0]) << 16 | to_byte(decoded[1]) << 8 | to_byte(decoded[2]);
}

// Positions vary in stream, stride and offset across cells, as do colours, crossing three fetch constant groups.
static uint32_t make_cells(Cell *cells)
{
	static const uint32_t position_streams[] = { 0, 3, 15 };
	static const uint32_t colour_streams[] = { 1, 2, 4 };
	uint32_t count = 0;
	for (uint32_t format = 0; format < XSDK_VERTEX_FORMATS; format++) {
		const uint32_t i = count;
		cells[count++] = (Cell){ specs[format].name, XSDK_VERTEX_FLOAT4, format, position_streams[i % 3],
			colour_streams[i % 3], 16 + 4 * (1 + i % 3), specs[format].bytes + 4 * (1 + i % 2), 4 * (i % 2),
			4 * (i % 2), 40 + i % COLOUR_FORMATS_PER_ROW * CELL_STEP, 60 + i / COLOUR_FORMATS_PER_ROW * ROW_STEP };
	}
	static const uint32_t position_formats[] = { XSDK_VERTEX_HALF2, XSDK_VERTEX_SHORT2N, XSDK_VERTEX_FLOAT2,
		XSDK_VERTEX_FLOAT3 };
	for (uint32_t i = 0; i < sizeof position_formats / sizeof position_formats[0]; i++) {
		const uint32_t format = position_formats[i];
		cells[count++] = (Cell){ specs[format].name, format, XSDK_VERTEX_UBYTE4N, 5, 6, specs[format].bytes + 4, 4, 4,
			0, 40 + i * CELL_STEP, 60 + 2 * ROW_STEP };
	}
	// Both in one stream, colour after position.
	cells[count++] = (Cell){ "interleaved", XSDK_VERTEX_FLOAT4, XSDK_VERTEX_HALF4, 0, 0, 24, 24, 0, 16,
		40 + 4 * CELL_STEP, 60 + 2 * ROW_STEP };
	return count;
}

static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

// A square's six corners in clip space, edges half a pixel before the pixels (xenia:src/xenia/gpu/draw_util.cc:837).
static void square_corners(const Cell *cell, uint32_t square, float corners[6][COMPONENTS])
{
	const float left = clip_x(cell->x + square % 2 * SQUARE_SIZE - 0.5f);
	const float right = clip_x(cell->x + square % 2 * SQUARE_SIZE + SQUARE_SIZE - 0.5f);
	const float top = clip_y(cell->y + square / 2 * SQUARE_SIZE - 0.5f);
	const float bottom = clip_y(cell->y + square / 2 * SQUARE_SIZE + SQUARE_SIZE - 0.5f);
	const float xs[6] = { left, right, left, left, right, right };
	const float ys[6] = { top, top, bottom, bottom, top, bottom };
	for (uint32_t i = 0; i < 6; i++) {
		corners[i][0] = xs[i];
		corners[i][1] = ys[i];
		corners[i][2] = 0.0f;
		corners[i][3] = 1.0f;
	}
}

static void put_words(volatile uint32_t *buffer, uint32_t offset_bytes, const uint32_t *words, uint32_t bytes)
{
	for (uint32_t i = 0; i < bytes / 4; i++)
		buffer[offset_bytes / 4 + i] = words[i];
}

// The cell's positions and colours, each vertex at its stream's stride and the attribute's offset.
static void fill_streams(const Cell *cell, uint32_t index, volatile uint32_t *positions, volatile uint32_t *colours)
{
	for (uint32_t square = 0; square < SQUARES; square++) {
		float corners[6][COMPONENTS];
		float colour[COMPONENTS];
		uint32_t words[COMPONENTS];
		square_corners(cell, square, corners);
		colour_of(index, cell->colour_format, square, colour);
		for (uint32_t i = 0; i < 6; i++) {
			const uint32_t vertex = square * 6 + i;
			encode(cell->position_format, corners[i], words);
			put_words(positions, vertex * cell->position_stride + cell->position_offset, words,
				specs[cell->position_format].bytes);
			encode(cell->colour_format, colour, words);
			put_words(
				colours, vertex * cell->colour_stride + cell->colour_offset, words, specs[cell->colour_format].bytes);
		}
	}
}

typedef struct {
	uint32_t cell;
	uint32_t x;
	uint32_t y;
	uint32_t argb;
} Point;

static uint32_t expected_argb(const Cell *cell, uint32_t index, uint32_t x, uint32_t y)
{
	return square_argb(cell, index, (y - cell->y) / SQUARE_SIZE * 2 + (x - cell->x) / SQUARE_SIZE);
}

// Each square's middle, and the pixel past the cell's bottom right.
static uint32_t make_points(const Cell *cells, uint32_t cell_count, Point *points)
{
	uint32_t count = 0;
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		for (uint32_t square = 0; square < SQUARES; square++) {
			const uint32_t x = cell->x + square % 2 * SQUARE_SIZE + SQUARE_SIZE / 2;
			const uint32_t y = cell->y + square / 2 * SQUARE_SIZE + SQUARE_SIZE / 2;
			points[count++] = (Point){ i, x, y, square_argb(cell, i, square) };
		}
		points[count++] = (Point){ i, cell->x + CELL_SIZE, cell->y + CELL_SIZE, CLEAR_ARGB };
	}
	return count;
}

// The largest difference in any channel, alpha too; a resolve stores the colour's bytes B, G, R, A.
static uint32_t error(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	uint32_t largest = 0;
	for (uint32_t shift = 0; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		const uint32_t difference = got > wanted ? got - wanted : wanted - got;
		largest = difference > largest ? difference : largest;
	}
	return largest;
}

static void log_miss(const Cell *cell, uint32_t frame, uint32_t x, uint32_t y, uint32_t stored, uint32_t argb)
{
	xsdk_log_write("vertex: frame %u: %s from %s at (%u, %u) held %08X, not %08X", (unsigned)frame,
		specs[cell->colour_format].name, specs[cell->position_format].name, (unsigned)x, (unsigned)y, (unsigned)stored,
		(unsigned)__builtin_bswap32(argb));
}

// Every pixel of the cell and the ring around it; returns how many are wrong.
static uint32_t cell_wrong(
	const Cell *cell, uint32_t index, volatile const uint32_t *pixels, uint32_t frame, uint32_t *logged)
{
	uint32_t wrong = 0;
	for (uint32_t y = cell->y - 1; y <= cell->y + CELL_SIZE; y++) {
		for (uint32_t x = cell->x - 1; x <= cell->x + CELL_SIZE; x++) {
			const bool inside = x >= cell->x && x < cell->x + CELL_SIZE && y >= cell->y && y < cell->y + CELL_SIZE;
			const uint32_t argb = inside ? expected_argb(cell, index, x, y) : CLEAR_ARGB;
			const uint32_t stored = pixels[tiled_index(x, y, WIDTH)];
			if (error(stored, argb) <= (inside ? TOLERANCE : 0))
				continue;
			if ((*logged)++ < MAX_MISSES_LOGGED)
				log_miss(cell, frame, x, y, stored, argb);
			wrong++;
		}
	}
	return wrong;
}

typedef struct {
	XsdkShader *vertex;
	const XsdkShader *pixel;
	const Cell *cells;
	uint32_t cell_count;
	const uint32_t *positions;
	const uint32_t *colours;
	volatile uint32_t *const *buffers;
	bool has_display;
} Scene;

static void draw_cells(const Scene *scene)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	for (uint32_t i = 0; i < scene->cell_count; i++) {
		const Cell *const cell = &scene->cells[i];
		xsdk_gpu_set_shaders(&scene->vertex[i], scene->pixel);
		xsdk_gpu_set_vertices(cell->position_stream, scene->positions[i], VERTICES * cell->position_stride);
		if (cell->colour_stream != cell->position_stream)
			xsdk_gpu_set_vertices(cell->colour_stream, scene->colours[i], VERTICES * cell->colour_stride);
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, VERTICES);
	}
}

// Each frame draws every cell into eDRAM, resolves into the buffer not on screen, and shows it.
static void draw_frames(const Scene *scene)
{
	static Point points[MAX_CELLS * POINTS_PER_CELL];
	const uint32_t point_count = make_points(scene->cells, scene->cell_count, points);
	uint32_t misses = 0;
	uint32_t frames_wrong = 0;
	uint32_t checked = 0;
	uint32_t pixels_wrong = 0;
	bool fences_landed = true;
	bool shown = true;
	for (uint32_t frame = 0; frame < FRAMES && fences_landed; frame++) {
		volatile uint32_t *const buffer = scene->buffers[frame % BUFFERS];
		const uint32_t address = xsdk_gpu_address(buffer);
		draw_cells(scene);
		xsdk_gpu_resolve(address, WIDTH, HEIGHT, CLEAR_ARGB);
		fences_landed = NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
		if (!fences_landed)
			xsdk_log_write("vertex: frame %u's fence didn't land", (unsigned)frame);
		// The first frame's background is whatever eDRAM held; later ones are the clear colour.
		if (frame > 0 && fences_landed) {
			bool frame_holds = true;
			for (uint32_t i = 0; i < point_count; i++) {
				const Point *const p = &points[i];
				const uint32_t stored = buffer[tiled_index(p->x, p->y, WIDTH)];
				if (error(stored, p->argb) <= (p->argb == CLEAR_ARGB ? 0 : TOLERANCE))
					continue;
				if (misses++ < MAX_MISSES_LOGGED)
					log_miss(&scene->cells[p->cell], frame, p->x, p->y, stored, p->argb);
				frame_holds = false;
			}
			frames_wrong += !frame_holds;
		}
		if (frame == FRAMES - 1 && fences_landed) {
			uint32_t logged = 0;
			for (uint32_t i = 0; i < scene->cell_count; i++) {
				const Cell *const cell = &scene->cells[i];
				const uint32_t ring = (CELL_SIZE + 2) * (CELL_SIZE + 2);
				const uint32_t wrong = cell_wrong(cell, i, buffer, frame, &logged);
				checked += ring;
				pixels_wrong += wrong;
				if (wrong)
					xsdk_log_write("vertex: %s colour from %s positions: %u of %u pixels wrong",
						specs[cell->colour_format].name, specs[cell->position_format].name, (unsigned)wrong,
						(unsigned)ring);
			}
		}
		if (scene->has_display)
			shown = shown && NT_SUCCESS(xsdk_display_show(address, WIDTH));
		(void)xsdk_thread_sleep(FRAME_MS);
	}
	xsdk_log_write("vertex: %u cells, %u points a frame; %u of %u frames checked held a wrong one, %u points wrong",
		(unsigned)scene->cell_count, (unsigned)point_count, (unsigned)frames_wrong, (unsigned)(FRAMES - 1),
		(unsigned)misses);
	xsdk_log_write("vertex: the last frame had %u of %u pixels wrong across the cells and their edges",
		(unsigned)pixels_wrong, (unsigned)checked);
	check("vertex: every frame's fence landed", fences_landed);
	check("vertex: every format, stream, stride and offset drew its colours",
		fences_landed && checked > 0 && frames_wrong == 0 && pixels_wrong == 0);
	if (scene->has_display) {
		check("vertex: every frame shown", shown);
		xsdk_log_write("vertex: the last frame stays on screen for %u ms for a capture", (unsigned)TEST_SHOW_MS);
		(void)xsdk_thread_sleep(TEST_SHOW_MS);
	}
}

// One vertex shader per cell, patched for its layout once, so no patch lands while the GPU runs a shader.
static bool load_shaders(XsdkShader *shaders, const Cell *cells, uint32_t cell_count)
{
	bool loaded = true;
	for (uint32_t i = 0; i < cell_count && loaded; i++) {
		const Cell *const cell = &cells[i];
		const XsdkVertexElement layout[] = {
			{ .usage = XSDK_USAGE_POSITION,
				.format = cell->position_format,
				.stream = cell->position_stream,
				.offset_bytes = cell->position_offset },
			{ .usage = XSDK_USAGE_COLOR,
				.format = cell->colour_format,
				.stream = cell->colour_stream,
				.offset_bytes = cell->colour_offset },
		};
		uint32_t strides[XSDK_VERTEX_STREAMS] = { 0 };
		strides[cell->position_stream] = cell->position_stride;
		strides[cell->colour_stream] = cell->colour_stride;
		loaded = NT_SUCCESS(xsdk_shader_load(&shaders[i], triangle_vertex_shader, sizeof triangle_vertex_shader)) &&
			NT_SUCCESS(xsdk_shader_set_vertex_layout(&shaders[i], layout, 2, strides));
	}
	return loaded;
}

void test_vertex(void)
{
	const NTSTATUS status = xsdk_gpu_open();
	check("vertex: GPU open", NT_SUCCESS(status));
	if (!NT_SUCCESS(status))
		return;
	uint32_t width = 0;
	uint32_t height = 0;
	const bool has_display =
		NT_SUCCESS(xsdk_display_open(XSDK_DISPLAY_720P, &width, &height)) && width == WIDTH && height == HEIGHT;
	check("vertex: display open at 1280x720", has_display);
	static Cell cells[MAX_CELLS];
	const uint32_t cell_count = make_cells(cells);
	static XsdkShader shaders[MAX_CELLS];
	XsdkShader pixel = { .microcode = NULL };
	const bool loaded = load_shaders(shaders, cells, cell_count) &&
		NT_SUCCESS(xsdk_shader_load(&pixel, triangle_pixel_shader, sizeof triangle_pixel_shader));
	check("vertex: a vertex shader patched for each cell's layout", loaded);
	static volatile uint32_t *position_buffers[MAX_CELLS];
	static volatile uint32_t *colour_buffers[MAX_CELLS];
	static uint32_t positions[MAX_CELLS];
	static uint32_t colours[MAX_CELLS];
	bool allocated = true;
	for (uint32_t i = 0; i < cell_count; i++) {
		const Cell *const cell = &cells[i];
		position_buffers[i] = xsdk_gpu_alloc(VERTICES * cell->position_stride);
		colour_buffers[i] = cell->colour_stream == cell->position_stream
			? position_buffers[i]
			: xsdk_gpu_alloc(VERTICES * cell->colour_stride);
		allocated = allocated && position_buffers[i] && colour_buffers[i];
		if (position_buffers[i] && colour_buffers[i]) {
			fill_streams(cell, i, position_buffers[i], colour_buffers[i]);
			__asm__ volatile("sync" ::: "memory");
			xsdk_gpu_invalidate(xsdk_gpu_address(position_buffers[i]), VERTICES * cell->position_stride);
			if (colour_buffers[i] != position_buffers[i])
				xsdk_gpu_invalidate(xsdk_gpu_address(colour_buffers[i]), VERTICES * cell->colour_stride);
			positions[i] = xsdk_gpu_address(position_buffers[i]);
			colours[i] = xsdk_gpu_address(colour_buffers[i]);
		}
	}
	__asm__ volatile("sync" ::: "memory");
	const uint32_t buffer_bytes = xsdk_gpu_resolve_bytes(WIDTH, HEIGHT);
	volatile uint32_t *const buffers[BUFFERS] = { xsdk_gpu_alloc(buffer_bytes), xsdk_gpu_alloc(buffer_bytes) };
	check("vertex: vertex and front buffers", allocated && buffers[0] && buffers[1]);
	// Without a display, as in Xenia, the frames still draw and are checked.
	if (loaded && allocated && buffers[0] && buffers[1]) {
		const Scene scene = { .vertex = shaders,
			.pixel = &pixel,
			.cells = cells,
			.cell_count = cell_count,
			.positions = positions,
			.colours = colours,
			.buffers = buffers,
			.has_display = has_display };
		draw_frames(&scene);
	}
	xsdk_display_close();
	for (uint32_t i = 0; i < BUFFERS; i++)
		xsdk_gpu_free(buffers[i]);
	for (uint32_t i = 0; i < cell_count; i++) {
		if (colour_buffers[i] != position_buffers[i])
			xsdk_gpu_free(colour_buffers[i]);
		xsdk_gpu_free(position_buffers[i]);
		if (shaders[i].microcode)
			xsdk_shader_free(&shaders[i]);
	}
	xsdk_shader_free(&pixel);
	check("vertex: GPU close", NT_SUCCESS(xsdk_gpu_close()));
}
