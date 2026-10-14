#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "hiz_shaders.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// The second discovery run for RB_HIZCONTROL: RB_HIZ_CLEAR's format, every bit again from a cleared buffer with
// less-than and greater-than draws, equal depths, stencil depth-fail operations, draws that don't write depth, depth
// written farther, and overlapping rectangles, each checked pixel by pixel against the CPU's depth and stencil test.
// Built against xsdk with xsdk-probe.patch, which lets the probe pick the resolve's RB_HIZCONTROL.

extern uint32_t xsdk_probe_resolve_hiz;

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_US = 3000000,
	CLEAR_ARGB = 0xFF202020,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	CHANNEL_TOLERANCE = 1,
	LADDER_COLUMNS = 64,
	DEPTH_24_MAX = 0xFFFFFF,
	MAX_STEPS = 64,
	MAX_TIMED = 4,
	MAX_PAIRS = 4,
	NO_LADDER = 0xFFFFFFFF,
	REPEAT = 4,
	RECTANGLES = 48,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc).
	RB_HIZCONTROL = 0x2203,
	RB_HIZ_CLEAR = 0x231C,
	HIZ_WRITE = 1,
	HIZ_TEST_WRITE = 3,
};

enum {
	SHADER_SHORT,
	SHADER_LONG,
	SHADER_KILL,
};

enum {
	STENCIL_OFF,
	// Stencil always passes; a pixel failing depth replaces it with 1.
	STENCIL_MARK_DEPTH_FAIL,
	// Depth off, stencil equal to 1.
	STENCIL_TEST_MARKED,
	STENCIL_MODES,
};

static const uint32_t NEAR_ARGB = 0xFF40E040;
static const uint32_t FAR_ARGB = 0xFFE04040;
static const uint32_t ALWAYS_ARGB = 0xFF4040E0;
static const uint32_t MID_ARGB = 0xFFE0E040;
static const uint32_t SECOND_ARGB = 0xFFE040E0;
static const uint32_t THIRD_ARGB = 0xFF40E0E0;
static const uint32_t MARKED_ARGB = 0xFFFFFFFF;

typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	float depth;
	uint8_t compare;
	bool write;
	uint8_t stencil;
	uint8_t shader;
	uint32_t argb;
	uint32_t hiz;
	uint8_t repeat;
	bool timed;
} Step;

typedef struct {
	uint32_t resolve_hiz;
	bool set_hiz_clear;
	uint32_t hiz_clear;
} Clear;

static const Clear PLAIN = { 0, false, 0 };
static const Clear FAR_CLEAR = { HIZ_WRITE, true, 0xFFFFFFFF };

static uint32_t model_depth[PIXELS];
static uint8_t model_stencil[PIXELS];
static uint32_t model_argb[PIXELS];
static uint32_t clear_depth24 = DEPTH_24_MAX;
static bool failed;

static XsdkShader vertex_shader;
static XsdkShader pixel_shaders[3];
static XsdkDepthStencilState depth_states[8][2];
static XsdkDepthStencilState stencil_states[STENCIL_MODES];
static uint32_t vertices_address;
static volatile uint32_t *buffer;
static uint32_t buffer_address;
static uint32_t current_shader = 0xFFFFFFFF;

static const XsdkVertexElement layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
};

static const float square[QUAD_VERTICES][VERTEX_FLOATS] = {
	{ 0.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 0.0f, 1.0f, 0.0f, 1.0f },
	{ 1.0f, 0.0f, 0.0f, 1.0f },
	{ 1.0f, 1.0f, 0.0f, 1.0f },
};

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static bool wait_fence(uint32_t fence, uint64_t *done_us)
{
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("hiz probe: fence %u didn't land", (unsigned)fence);
			failed = true;
			return false;
		}
	}
	if (done_us)
		*done_us = now_us();
	return true;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

static uint32_t depth24(float depth)
{
	return (uint32_t)(depth * (double)DEPTH_24_MAX + 0.5);
}

static bool passes(uint32_t compare, uint32_t incoming, uint32_t stored)
{
	switch (compare) {
	case XSDK_COMPARE_NEVER:
		return false;
	case XSDK_COMPARE_LESS:
		return incoming < stored;
	case XSDK_COMPARE_EQUAL:
		return incoming == stored;
	case XSDK_COMPARE_LESS_EQUAL:
		return incoming <= stored;
	case XSDK_COMPARE_GREATER:
		return incoming > stored;
	case XSDK_COMPARE_NOT_EQUAL:
		return incoming != stored;
	case XSDK_COMPARE_GREATER_EQUAL:
		return incoming >= stored;
	default:
		return true;
	}
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

static void model_reset(void)
{
	for (uint32_t i = 0; i < PIXELS; i++) {
		model_depth[i] = clear_depth24;
		model_stencil[i] = 0;
		model_argb[i] = CLEAR_ARGB;
	}
}

static void model_pixel(const Step *step, uint32_t i, uint32_t z)
{
	if (step->stencil == STENCIL_TEST_MARKED) {
		if (model_stencil[i] == 1)
			model_argb[i] = step->argb;
		return;
	}
	if (!passes(step->compare, z, model_depth[i])) {
		if (step->stencil == STENCIL_MARK_DEPTH_FAIL)
			model_stencil[i] = 1;
		return;
	}
	model_argb[i] = step->argb;
	if (step->write)
		model_depth[i] = z;
}

static void model_draw(const Step *step)
{
	if (step->shader == SHADER_KILL)
		return;
	const uint32_t z = depth24(step->depth);
	for (uint32_t y = step->y; y < step->y + step->height; y++)
		for (uint32_t x = step->x; x < step->x + step->width; x++)
			model_pixel(step, y * WIDTH + x, z);
}

static void begin_target(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	// c0 0 for the long shader's multiply-adds, c1 > c2 for the killing shader.
	const float constants[3][4] = { { 0, 0, 0, 0 }, { 1, 1, 1, 1 }, { 0, 0, 0, 0 } };
	xsdk_gpu_set_pixel_constants(0, &constants[0][0], 3);
	current_shader = 0xFFFFFFFF;
}

static void put_step(const Step *step)
{
	if (current_shader != step->shader) {
		xsdk_gpu_set_shaders(&vertex_shader, &pixel_shaders[step->shader]);
		current_shader = step->shader;
	}
	if (step->stencil == STENCIL_OFF)
		xsdk_gpu_set_depth_stencil(&depth_states[step->compare][step->write]);
	else
		xsdk_gpu_set_depth_stencil(&stencil_states[step->stencil]);
	put_register(RB_HIZCONTROL, step->hiz);
	const float left = (step->x - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float right = (step->x + step->width - 0.5f) / (WIDTH / 2.0f) - 1.0f;
	const float top = 1.0f - (step->y - 0.5f) / (HEIGHT / 2.0f);
	const float bottom = 1.0f - (step->y + step->height - 0.5f) / (HEIGHT / 2.0f);
	const float vertex[3][4] = {
		{ right - left, bottom - top, 1.0f, 1.0f },
		{ left, top, step->depth, 0.0f },
		{ unorm(step->argb >> 16), unorm(step->argb >> 8), unorm(step->argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	for (uint32_t i = 0; i < step->repeat; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

static uint32_t ladder_argb(uint32_t column)
{
	return 0xFF000000 | (column * 4) << 16 | 0x8000 | (0xFF - column * 4);
}

static Step ladder_step(uint32_t column, uint32_t hiz)
{
	return (Step){ column * (WIDTH / LADDER_COLUMNS), 0, WIDTH / LADDER_COLUMNS, HEIGHT,
		(column + 0.5f) / LADDER_COLUMNS, XSDK_COMPARE_LESS, false, STENCIL_OFF, SHADER_SHORT, ladder_argb(column), hiz,
		1, false };
}

static Step full(float depth, uint32_t compare, bool write, uint32_t shader, uint32_t argb, uint32_t hiz)
{
	return (Step){ 0, 0, WIDTH, HEIGHT, depth, compare, write, STENCIL_OFF, shader, argb, hiz, 1, false };
}

static Step timed(Step step, uint32_t repeat)
{
	step.repeat = repeat;
	step.timed = true;
	return step;
}

static bool close_enough(uint32_t stored, uint32_t argb)
{
	const uint32_t want = __builtin_bswap32(argb);
	for (uint32_t shift = 8; shift <= 24; shift += 8) {
		const uint32_t got = stored >> shift & 0xFF;
		const uint32_t wanted = want >> shift & 0xFF;
		if ((got > wanted ? got - wanted : wanted - got) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

typedef struct {
	uint32_t want;
	uint32_t got;
	uint32_t count;
} Pair;

static void log_ladder(const char *name, const uint32_t *column_drawn)
{
	char digits[LADDER_COLUMNS + 1];
	uint32_t first_dark = LADDER_COLUMNS;
	for (uint32_t i = 0; i < LADDER_COLUMNS; i++) {
		const uint32_t sixteenths = column_drawn[i] * 16 / (WIDTH / LADDER_COLUMNS * HEIGHT);
		digits[i] = "0123456789ABCDEFG"[sixteenths];
		if (sixteenths < 16 && first_dark == LADDER_COLUMNS)
			first_dark = i;
	}
	digits[LADDER_COLUMNS] = '\0';
	xsdk_log_write("hiz probe: %s: ladder at (i + 0.5) / 64, sixteenths drawn: %s; first not whole: %u", name, digits,
		(unsigned)first_dark);
}

static uint32_t check(const char *name, const uint32_t *times_us, const Step *const *timed_steps, uint32_t timed_count,
	bool ladder)
{
	uint32_t wrong = 0;
	uint32_t left = WIDTH;
	uint32_t top = HEIGHT;
	uint32_t right = 0;
	uint32_t bottom = 0;
	Pair pairs[MAX_PAIRS] = { 0 };
	static uint32_t column_drawn[LADDER_COLUMNS];
	memset(column_drawn, 0, sizeof column_drawn);
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = buffer[tiled_index(x, y, WIDTH)];
			const uint32_t want = model_argb[y * WIDTH + x];
			if (ladder) {
				const uint32_t column = x / (WIDTH / LADDER_COLUMNS);
				column_drawn[column] += close_enough(stored, ladder_argb(column));
			}
			if (close_enough(stored, want))
				continue;
			wrong++;
			left = x < left ? x : left;
			top = y < top ? y : top;
			right = x > right ? x : right;
			bottom = y > bottom ? y : bottom;
			const uint32_t got = __builtin_bswap32(stored) | 0xFF000000;
			for (uint32_t i = 0; i < MAX_PAIRS; i++) {
				if (!pairs[i].count) {
					pairs[i] = (Pair){ want, got, 1 };
					break;
				}
				if (pairs[i].want == want && pairs[i].got == got) {
					pairs[i].count++;
					break;
				}
			}
		}
	}
	char line[384];
	int length = snprintf(line, sizeof line, "hiz probe: %s: %u of %u pixels wrong", name, (unsigned)wrong, PIXELS);
	if (wrong)
		length += snprintf(line + length, sizeof line - length, " in (%u, %u) to (%u, %u)", (unsigned)left,
			(unsigned)top, (unsigned)right, (unsigned)bottom);
	for (uint32_t i = 0; i < MAX_PAIRS && pairs[i].count && length < (int)sizeof line; i++)
		length += snprintf(line + length, sizeof line - length, "; %u %08X for %08X", (unsigned)pairs[i].count,
			(unsigned)pairs[i].got, (unsigned)pairs[i].want);
	xsdk_log_write("%s", line);
	for (uint32_t i = 0; i < timed_count; i++) {
		const Step *const s = timed_steps[i];
		const uint64_t pixels = (uint64_t)s->width * s->height * s->repeat;
		xsdk_log_write("hiz probe: %s: %u draws of %ux%u at %.3f took %u us, %u Mpixel/s", name, (unsigned)s->repeat,
			(unsigned)s->width, (unsigned)s->height, (double)s->depth, (unsigned)times_us[i],
			(unsigned)(times_us[i] ? pixels / times_us[i] : 0));
	}
	if (ladder)
		log_ladder(name, column_drawn);
	return wrong;
}

static void set_depth_clear(float depth)
{
	clear_depth24 = depth24(depth);
	xsdk_gpu_set_depth_clear(depth, 0);
}

// Draws the steps, then the ladder if asked, then resolves with the clear and checks every pixel.
static uint32_t run_phase(
	const char *name, const Step *steps, uint32_t step_count, uint32_t ladder_hiz, const Clear *clear)
{
	if (failed)
		return 0;
	uint32_t times_us[MAX_TIMED] = { 0 };
	const Step *timed_steps[MAX_TIMED];
	uint32_t timed_count = 0;
	for (uint32_t i = 0; i < step_count && !failed; i++) {
		const Step *const step = &steps[i];
		if (step->timed && timed_count < MAX_TIMED) {
			uint64_t start;
			uint64_t end;
			if (!wait_fence(xsdk_gpu_fence(), &start))
				return 0;
			put_step(step);
			if (!wait_fence(xsdk_gpu_fence(), &end))
				return 0;
			timed_steps[timed_count] = step;
			times_us[timed_count++] = (uint32_t)(end - start);
		} else {
			put_step(step);
		}
		model_draw(step);
	}
	if (ladder_hiz != NO_LADDER) {
		for (uint32_t i = 0; i < LADDER_COLUMNS; i++) {
			const Step step = ladder_step(i, ladder_hiz);
			put_step(&step);
			model_draw(&step);
		}
	}
	xsdk_probe_resolve_hiz = clear->resolve_hiz;
	if (clear->set_hiz_clear)
		put_register(RB_HIZ_CLEAR, clear->hiz_clear);
	xsdk_gpu_resolve(buffer_address, WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_probe_resolve_hiz = 0;
	if (!wait_fence(xsdk_gpu_fence(), NULL))
		return 0;
	const uint32_t wrong = check(name, times_us, timed_steps, timed_count, ladder_hiz != NO_LADDER);
	model_reset();
	begin_target();
	return wrong;
}

static void reset_far(void)
{
	run_phase("reset", NULL, 0, NO_LADDER, &FAR_CLEAR);
}

// The resolve sets the buffer from RB_HIZ_CLEAR with RB_HIZCONTROL 1; the ladder reads what it holds.
static void clear_format(void)
{
	static const uint32_t values[] = { 0x7FF, 0xFFF, 0x400, 0x200, 0x600, 0x700, 0x7C0, 0x3FF, 0x401, 0x100, 0x080,
		0x800, 0xA00, 0xC00, 0x1000, 0x1400, 0x2000, 0x4000, 0x8000, 0x7FFF };
	for (uint32_t i = 0; i < sizeof values / sizeof values[0]; i++) {
		char name[64];
		snprintf(name, sizeof name, "RB_HIZ_CLEAR %08X", (unsigned)values[i]);
		run_phase(name, NULL, 0, NO_LADDER, &(Clear){ HIZ_WRITE, true, values[i] });
		run_phase(name, NULL, 0, 2, &PLAIN);
	}
	reset_far();
}

static void less_hidden(const char *name, uint32_t hiz)
{
	const Step steps[] = {
		full(0.3f, XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, hiz),
		timed(full(0.9f, XSDK_COMPARE_LESS, true, SHADER_LONG, FAR_ARGB, hiz), REPEAT),
	};
	run_phase(name, steps, 2, NO_LADDER, &PLAIN);
}

// After a clear to 0: 0.6, then hidden ones at 0.3 timed, then 0.8 over part of them.
static void greater_hidden(const char *name, uint32_t compare, uint32_t hiz)
{
	const Step steps[] = {
		full(0.6f, compare, true, SHADER_SHORT, NEAR_ARGB, hiz),
		timed(full(0.3f, compare, true, SHADER_LONG, FAR_ARGB, hiz), REPEAT),
		{ 320, 180, 640, 360, 0.8f, compare, true, STENCIL_OFF, SHADER_SHORT, MID_ARGB, hiz, 1, false },
	};
	run_phase(name, steps, 3, NO_LADDER, &PLAIN);
}

static void bit_test(uint32_t extra)
{
	char name[96];
	reset_far();
	snprintf(name, sizeof name, "less, RB_HIZCONTROL %08X", (unsigned)(HIZ_TEST_WRITE | extra));
	less_hidden(name, HIZ_TEST_WRITE | extra);
	set_depth_clear(0.0f);
	snprintf(name, sizeof name, "clear to 0 with RB_HIZCONTROL %08X", (unsigned)(HIZ_WRITE | extra));
	run_phase(name, NULL, 0, NO_LADDER, &(Clear){ HIZ_WRITE | extra, true, 0 });
	snprintf(name, sizeof name, "greater, RB_HIZCONTROL %08X", (unsigned)(HIZ_TEST_WRITE | extra));
	greater_hidden(name, XSDK_COMPARE_GREATER, HIZ_TEST_WRITE | extra);
	set_depth_clear(1.0f);
	reset_far();
}

static void bits(void)
{
	bit_test(0);
	for (uint32_t bit = 2; bit < 32 && !failed; bit++)
		bit_test(1u << bit);
}

static void equal_depths(void)
{
	reset_far();
	const Step steps[] = {
		full(0.5f, XSDK_COMPARE_LESS_EQUAL, true, SHADER_SHORT, NEAR_ARGB, HIZ_TEST_WRITE),
		full(0.5f, XSDK_COMPARE_LESS_EQUAL, true, SHADER_SHORT, SECOND_ARGB, HIZ_TEST_WRITE),
		{ 0, 0, 640, HEIGHT, 0.5f, XSDK_COMPARE_EQUAL, true, STENCIL_OFF, SHADER_SHORT, THIRD_ARGB, HIZ_TEST_WRITE, 1,
			false },
	};
	run_phase("less-equal then equal at 0.5", steps, 3, NO_LADDER, &FAR_CLEAR);
}

// A near quad, then a far one that fails depth everywhere and marks stencil there, then a quad drawn where marked.
static void stencil_depth_fail(void)
{
	static const uint32_t hiz_values[] = { HIZ_TEST_WRITE, HIZ_WRITE };
	static const char *const names[] = { "stencil depth-fail, RB_HIZCONTROL 3", "stencil depth-fail, RB_HIZCONTROL 1" };
	for (uint32_t i = 0; i < 2; i++) {
		reset_far();
		const Step steps[] = {
			full(0.3f, XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, HIZ_TEST_WRITE),
			{ 0, 0, WIDTH, HEIGHT, 0.6f, XSDK_COMPARE_LESS, true, STENCIL_MARK_DEPTH_FAIL, SHADER_SHORT, FAR_ARGB,
				hiz_values[i], 1, false },
			{ 0, 0, WIDTH, HEIGHT, 0.0f, XSDK_COMPARE_ALWAYS, false, STENCIL_TEST_MARKED, SHADER_SHORT, MARKED_ARGB, 0, 1,
				false },
		};
		run_phase(names[i], steps, 3, NO_LADDER, &FAR_CLEAR);
	}
}

// A near quad that tests but doesn't write depth, then a far one that must show.
static void no_depth_write(void)
{
	reset_far();
	const Step steps[] = {
		full(0.3f, XSDK_COMPARE_LESS, false, SHADER_SHORT, NEAR_ARGB, HIZ_TEST_WRITE),
		full(0.6f, XSDK_COMPARE_LESS, true, SHADER_SHORT, FAR_ARGB, HIZ_TEST_WRITE),
	};
	run_phase("no depth write, RB_HIZCONTROL 3", steps, 2, NO_LADDER, &FAR_CLEAR);
}

// Depth written farther with always, then a less-than quad between; where it's wrong shows the buffer's tiles.
static void farther_writes(void)
{
	static const float farther[] = { 0.7f, 1.0f, 0.95f };
	for (uint32_t i = 0; i < 3; i++) {
		reset_far();
		char name[64];
		snprintf(name, sizeof name, "always at %.2f with RB_HIZCONTROL 1", (double)farther[i]);
		const Step steps[] = {
			full(0.3f, XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, HIZ_TEST_WRITE),
			full(farther[i], XSDK_COMPARE_ALWAYS, true, SHADER_SHORT, ALWAYS_ARGB, HIZ_WRITE),
			full(0.5f, XSDK_COMPARE_LESS, true, SHADER_SHORT, MID_ARGB, HIZ_TEST_WRITE),
		};
		run_phase(name, steps, 3, NO_LADDER, &FAR_CLEAR);
	}
	// The same with a rectangle, its edges inside the buffer's tiles.
	reset_far();
	const Step steps[] = {
		full(0.3f, XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, HIZ_TEST_WRITE),
		{ 101, 53, 803, 411, 0.7f, XSDK_COMPARE_ALWAYS, true, STENCIL_OFF, SHADER_SHORT, ALWAYS_ARGB, HIZ_WRITE, 1,
			false },
		full(0.5f, XSDK_COMPARE_LESS, true, SHADER_SHORT, MID_ARGB, HIZ_TEST_WRITE),
	};
	run_phase("always at 0.7 in a rectangle with RB_HIZCONTROL 1", steps, 3, NO_LADDER, &FAR_CLEAR);
}

static uint32_t random_state = 12345;

static uint32_t random_below(uint32_t limit)
{
	random_state = random_state * 1103515245 + 12345;
	return (random_state >> 8) % limit;
}

// Overlapping rectangles at random places and depths, less-than and less-equal, in the order drawn.
static void rectangles(uint32_t hiz, const char *name)
{
	static Step steps[RECTANGLES];
	random_state = 12345;
	for (uint32_t i = 0; i < RECTANGLES; i++) {
		const uint32_t width = 1 + random_below(500);
		const uint32_t height = 1 + random_below(300);
		const uint32_t x = random_below(WIDTH - width + 1);
		const uint32_t y = random_below(HEIGHT - height + 1);
		const float depth = (1 + random_below(999)) / 1000.0f;
		const uint32_t compare = i % 3 ? XSDK_COMPARE_LESS : XSDK_COMPARE_LESS_EQUAL;
		const uint32_t argb = 0xFF000000 | (random_below(0x1000000) & 0xF0F0F0) | 0x080808;
		steps[i] = (Step){ x, y, width, height, depth, compare, true, STENCIL_OFF, SHADER_SHORT, argb, hiz, 1, false };
	}
	reset_far();
	run_phase(name, steps, RECTANGLES, NO_LADDER, &FAR_CLEAR);
	// Again with a clear to 0.5, and the buffer cleared to far.
	set_depth_clear(0.5f);
	run_phase("clear to 0.5", NULL, 0, NO_LADDER, &FAR_CLEAR);
	char second[96];
	snprintf(second, sizeof second, "%s, over a clear to 0.5", name);
	run_phase(second, steps, RECTANGLES, NO_LADDER, &FAR_CLEAR);
	set_depth_clear(1.0f);
	reset_far();
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shaders[SHADER_SHORT], triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shaders[SHADER_LONG], long_pixel_shader, sizeof long_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shaders[SHADER_KILL], kill_pixel_shader, sizeof kill_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	for (uint32_t compare = 0; compare < 8; compare++) {
		for (uint32_t write = 0; write < 2; write++) {
			const XsdkDepthStencil setting = { .depth_test = true, .depth_write = write, .depth_compare = compare };
			xsdk_gpu_depth_stencil_state(&depth_states[compare][write], &setting);
		}
	}
	const XsdkDepthStencil mark = { .depth_test = true,
		.depth_write = true,
		.depth_compare = XSDK_COMPARE_LESS,
		.stencil_test = true,
		.stencil_compare = XSDK_COMPARE_ALWAYS,
		.stencil_fail = XSDK_STENCIL_KEEP,
		.stencil_depth_fail = XSDK_STENCIL_REPLACE,
		.stencil_pass = XSDK_STENCIL_KEEP,
		.stencil_reference = 1,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0xFF };
	const XsdkDepthStencil marked = { .stencil_test = true,
		.stencil_compare = XSDK_COMPARE_EQUAL,
		.stencil_fail = XSDK_STENCIL_KEEP,
		.stencil_depth_fail = XSDK_STENCIL_KEEP,
		.stencil_pass = XSDK_STENCIL_KEEP,
		.stencil_reference = 1,
		.stencil_read_mask = 0xFF,
		.stencil_write_mask = 0 };
	const XsdkDepthStencil off = { .depth_test = false };
	xsdk_gpu_depth_stencil_state(&stencil_states[STENCIL_OFF], &off);
	xsdk_gpu_depth_stencil_state(&stencil_states[STENCIL_MARK_DEPTH_FAIL], &mark);
	xsdk_gpu_depth_stencil_state(&stencil_states[STENCIL_TEST_MARKED], &marked);
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	buffer = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertices || !buffer)
		return false;
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(vertices);
	buffer_address = xsdk_gpu_address(buffer);
	xsdk_gpu_invalidate(vertices_address, sizeof square);
	return true;
}

int main(void)
{
	xsdk_log_write("hiz probe 2");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	set_depth_clear(1.0f);
	model_reset();
	begin_target();
	// eDRAM's colour is whatever was there before this first resolve.
	run_phase("first clear", NULL, 0, NO_LADDER, &FAR_CLEAR);
	clear_format();
	equal_depths();
	stencil_depth_fail();
	no_depth_write();
	farther_writes();
	rectangles(HIZ_TEST_WRITE, "rectangles, RB_HIZCONTROL 3");
	rectangles(0, "rectangles, RB_HIZCONTROL 0");
	bits();
	xsdk_log_write("hiz probe: %s", failed ? "stopped early" : "done");
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
