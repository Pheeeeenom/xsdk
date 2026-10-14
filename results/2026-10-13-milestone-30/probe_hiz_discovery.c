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

// What RB_HIZCONTROL's bits do, and what keeps the hierarchical buffer right: draws checked pixel by pixel against
// the CPU's depth test, hidden pixels timed, and columns at 16 depths read back what the buffer holds. Built against
// xsdk with xsdk-probe.patch, which lets the probe pick the resolve's RB_HIZCONTROL.

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
	LADDER_COLUMNS = 16,
	DEPTH_24_MAX = 0xFFFFFF,
	MAX_STEPS = 48,
	MAX_TIMED = 4,
	MAX_PAIRS = 4,
	NO_LADDER = 0xFFFFFFFF,
	FAR_REPEAT = 8,
	BIT_REPEAT = 4,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc).
	RB_SURFACE_INFO = 0x2000,
	RB_DEPTH_INFO = 0x2002,
	RB_DEPTHCONTROL = 0x2200,
	RB_HIZCONTROL = 0x2203,
	RB_COPY_CONTROL = 0x2318,
	RB_HIZ_CLEAR = 0x231C,
	RB_DEPTH_CLEAR = 0x231D,
};

enum {
	SHADER_SHORT,
	SHADER_LONG,
	SHADER_KILL,
};

static const uint32_t NEAR_ARGB = 0xFF40E040;
static const uint32_t FAR_ARGB = 0xFFE04040;
static const uint32_t ALWAYS_ARGB = 0xFF4040E0;
static const uint32_t MID_ARGB = 0xFFE0E040;
static const uint32_t KILL_ARGB = 0xFFE040E0;
static const uint32_t RESET_ARGB = 0xFF40E0E0;

typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t width;
	uint16_t height;
	float depth;
	uint8_t compare;
	bool write;
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

static volatile uint32_t *const gpu_registers = (volatile uint32_t *)0x7FC80000;
static uint32_t model_depth[PIXELS];
static uint32_t model_argb[PIXELS];
static uint32_t clear_depth24 = DEPTH_24_MAX;
static bool failed;
static float next_near_depth = 0.45f;

static XsdkShader vertex_shader;
static XsdkShader pixel_shaders[3];
static XsdkDepthStencilState depth_states[8][2];
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

// Spins, so the time is the GPU's to within a few microseconds.
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
		model_argb[i] = CLEAR_ARGB;
	}
}

static void model_draw(const Step *step)
{
	if (step->shader == SHADER_KILL)
		return;
	const uint32_t z = depth24(step->depth);
	for (uint32_t y = step->y; y < step->y + step->height; y++) {
		for (uint32_t x = step->x; x < step->x + step->width; x++) {
			const uint32_t i = y * WIDTH + x;
			if (!passes(step->compare, z, model_depth[i]))
				continue;
			model_argb[i] = step->argb;
			if (step->write)
				model_depth[i] = z;
		}
	}
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

// The constant vertex shader places the unit square, edges half a pixel early, at the depth in c1.z, coloured by c2.
static void put_step(const Step *step)
{
	if (current_shader != step->shader) {
		xsdk_gpu_set_shaders(&vertex_shader, &pixel_shaders[step->shader]);
		current_shader = step->shader;
	}
	xsdk_gpu_set_depth_stencil(&depth_states[step->compare][step->write]);
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
	return 0xFF000000 | (column * 16) << 16 | 0x8000 | (0xFF - column * 16);
}

static Step ladder_step(uint32_t column, uint32_t hiz)
{
	return (Step){ column * (WIDTH / LADDER_COLUMNS), 0, WIDTH / LADDER_COLUMNS, HEIGHT,
		(column + 0.5f) / LADDER_COLUMNS, XSDK_COMPARE_LESS, false, SHADER_SHORT, ladder_argb(column), hiz, 1, false };
}

static Step full(float depth, uint32_t compare, bool write, uint32_t shader, uint32_t argb, uint32_t hiz)
{
	return (Step){ 0, 0, WIDTH, HEIGHT, depth, compare, write, shader, argb, hiz, 1, false };
}

static Step timed(Step step, uint32_t repeat)
{
	step.repeat = repeat;
	step.timed = true;
	return step;
}

static float take_near_depth(void)
{
	const float depth = next_near_depth;
	next_near_depth -= 0.004f;
	return depth;
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

static void check(const char *name, const uint32_t *times_us, const Step *const *timed_steps, uint32_t timed_count,
	bool ladder)
{
	uint32_t wrong = 0;
	Pair pairs[MAX_PAIRS] = { 0 };
	uint32_t column_drawn[LADDER_COLUMNS] = { 0 };
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
	char line[320];
	int length = snprintf(line, sizeof line, "hiz probe: %s: %u of %u pixels wrong", name, (unsigned)wrong, PIXELS);
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
	if (ladder) {
		char digits[LADDER_COLUMNS + 1];
		for (uint32_t i = 0; i < LADDER_COLUMNS; i++) {
			const uint32_t sixteenths = column_drawn[i] * 16 / (WIDTH / LADDER_COLUMNS * HEIGHT);
			digits[i] = "0123456789ABCDEFG"[sixteenths];
		}
		digits[LADDER_COLUMNS] = '\0';
		xsdk_log_write("hiz probe: %s: ladder, sixteenths drawn at depths 1/32 to 31/32: %s", name, digits);
	}
}

// Draws the steps, then the ladder if asked, then resolves with the clear and checks every pixel.
static void run_phase(const char *name, const Step *steps, uint32_t step_count, uint32_t ladder_hiz, const Clear *clear)
{
	if (failed)
		return;
	uint32_t times_us[MAX_TIMED] = { 0 };
	const Step *timed_steps[MAX_TIMED];
	uint32_t timed_count = 0;
	for (uint32_t i = 0; i < step_count && !failed; i++) {
		const Step *const step = &steps[i];
		if (step->timed && timed_count < MAX_TIMED) {
			uint64_t start;
			uint64_t end;
			if (!wait_fence(xsdk_gpu_fence(), &start))
				return;
			put_step(step);
			if (!wait_fence(xsdk_gpu_fence(), &end))
				return;
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
		return;
	check(name, times_us, timed_steps, timed_count, ladder_hiz != NO_LADDER);
	model_reset();
	begin_target();
}

static void ladder(const char *name, uint32_t hiz)
{
	run_phase(name, NULL, 0, hiz, &PLAIN);
}

// A near quad, then far quads through the long shader, all with RB_HIZCONTROL hiz.
static void hidden_pixels(const char *name, uint32_t hiz, uint32_t repeat)
{
	const Step steps[] = {
		full(take_near_depth(), XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, hiz),
		timed(full(0.9f, XSDK_COMPARE_LESS, true, SHADER_LONG, FAR_ARGB, hiz), repeat),
	};
	run_phase(name, steps, 2, NO_LADDER, &PLAIN);
}

static void xenia_bits(void)
{
	static const char *const names[] = { "hidden pixels, RB_HIZCONTROL 0", "hidden pixels, RB_HIZCONTROL 1",
		"hidden pixels, RB_HIZCONTROL 2", "hidden pixels, RB_HIZCONTROL 3" };
	for (uint32_t hiz = 0; hiz < 4; hiz++)
		hidden_pixels(names[hiz], hiz, FAR_REPEAT);
	ladder("after the RB_HIZCONTROL 3 run and a plain resolve", 2);
}

// A near quad with RB_HIZCONTROL 3, then the clear, then the ladder reads what the buffer holds.
static void clear_method(const char *name, const Clear *clear, bool reset_draw)
{
	char phase_name[96];
	const Step near = full(take_near_depth(), XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, 3);
	snprintf(phase_name, sizeof phase_name, "clear %s, before", name);
	run_phase(phase_name, &near, 1, NO_LADDER, clear);
	snprintf(phase_name, sizeof phase_name, "clear %s, after", name);
	const Step reset = full(1.0f, XSDK_COMPARE_ALWAYS, true, SHADER_SHORT, RESET_ARGB, 1);
	run_phase(phase_name, &reset, reset_draw ? 1 : 0, 2, &PLAIN);
}

static void clears(void)
{
	clear_method("plain", &PLAIN, false);
	clear_method("plain, RB_HIZ_CLEAR FFFFFFFF", &(Clear){ 0, true, 0xFFFFFFFF }, false);
	clear_method("plain, then always at 1 with RB_HIZCONTROL 1", &PLAIN, true);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR FFFFFFFF", &(Clear){ 1, true, 0xFFFFFFFF }, false);
	clear_method("resolve RB_HIZCONTROL 3, RB_HIZ_CLEAR FFFFFFFF", &(Clear){ 3, true, 0xFFFFFFFF }, false);
	clear_method("resolve RB_HIZCONTROL 2, RB_HIZ_CLEAR FFFFFFFF", &(Clear){ 2, true, 0xFFFFFFFF }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR FFFFFF00", &(Clear){ 1, true, 0xFFFFFF00 }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 00FFFFFF", &(Clear){ 1, true, 0x00FFFFFF }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 7FFFFF00", &(Clear){ 1, true, 0x7FFFFF00 }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 007FFFFF", &(Clear){ 1, true, 0x007FFFFF }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 3F000000", &(Clear){ 1, true, 0x3F000000 }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 00002000", &(Clear){ 1, true, 0x00002000 }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR 0", &(Clear){ 1, true, 0 }, false);
	clear_method("resolve RB_HIZCONTROL 1, RB_HIZ_CLEAR FFFFFFFF again", &(Clear){ 1, true, 0xFFFFFFFF }, false);
}

// Killed pixels must leave the buffer as it was: a killing near quad, then a far one.
static void kills(void)
{
	static const uint32_t hiz_values[] = { 3, 2 };
	static const char *const names[] = { "killed near quad, RB_HIZCONTROL 3", "killed near quad, RB_HIZCONTROL 2" };
	for (uint32_t i = 0; i < 2; i++) {
		const Step steps[] = {
			full(take_near_depth(), XSDK_COMPARE_LESS, true, SHADER_KILL, KILL_ARGB, hiz_values[i]),
			full(0.9f, XSDK_COMPARE_LESS, true, SHADER_SHORT, FAR_ARGB, 3),
		};
		run_phase(names[i], steps, 2, NO_LADDER, &PLAIN);
	}
}

// Depth written farther: near, then always at 0.7 with each RB_HIZCONTROL, then a less-than quad at 0.5 that shows.
static void farther_writes(void)
{
	static const uint32_t hiz_values[] = { 1, 3, 0 };
	static const char *const names[] = { "always at 0.7 with RB_HIZCONTROL 1", "always at 0.7 with RB_HIZCONTROL 3",
		"always at 0.7 with RB_HIZCONTROL 0" };
	for (uint32_t i = 0; i < 3; i++) {
		const Step steps[] = {
			full(take_near_depth(), XSDK_COMPARE_LESS, true, SHADER_SHORT, NEAR_ARGB, 3),
			full(0.7f, XSDK_COMPARE_ALWAYS, true, SHADER_SHORT, ALWAYS_ARGB, hiz_values[i]),
			full(0.5f, XSDK_COMPARE_LESS, true, SHADER_SHORT, MID_ARGB, 3),
		};
		run_phase(names[i], steps, 3, NO_LADDER, &PLAIN);
	}
}

// Greater-than draws after a clear to 0: 0.6, then hidden ones at 0.3 timed, then 0.8 over them.
static void greater(void)
{
	clear_depth24 = 0;
	xsdk_gpu_set_depth_clear(0.0f, 0);
	run_phase("clear to 0", NULL, 0, NO_LADDER, &PLAIN);
	static const uint32_t hiz_values[] = { 0, 3 };
	static const char *const names[] = { "greater, RB_HIZCONTROL 0", "greater, RB_HIZCONTROL 3" };
	for (uint32_t i = 0; i < 2; i++) {
		const Step steps[] = {
			full(0.6f, XSDK_COMPARE_GREATER, true, SHADER_SHORT, NEAR_ARGB, hiz_values[i]),
			timed(full(0.3f, XSDK_COMPARE_GREATER, true, SHADER_LONG, FAR_ARGB, hiz_values[i]), BIT_REPEAT),
			{ 320, 180, 640, 360, 0.8f, XSDK_COMPARE_GREATER, true, SHADER_SHORT, MID_ARGB, hiz_values[i], 1, false },
		};
		run_phase(names[i], steps, 3, NO_LADDER, &PLAIN);
	}
	clear_depth24 = DEPTH_24_MAX;
	xsdk_gpu_set_depth_clear(1.0f, 0);
	run_phase("clear to 1", NULL, 0, NO_LADDER, &PLAIN);
}

static void single_bits(void)
{
	for (uint32_t bit = 2; bit < 32 && !failed; bit++) {
		char name[64];
		snprintf(name, sizeof name, "hidden pixels, RB_HIZCONTROL %08X", (unsigned)(3u | 1u << bit));
		hidden_pixels(name, 3u | 1u << bit, BIT_REPEAT);
	}
}

static void log_registers(void)
{
	xsdk_log_write("hiz probe: as XAM left them: RB_HIZCONTROL %08X, RB_HIZ_CLEAR %08X, RB_DEPTHCONTROL %08X, "
				   "RB_SURFACE_INFO %08X, RB_DEPTH_INFO %08X, RB_DEPTH_CLEAR %08X, RB_COPY_CONTROL %08X",
		(unsigned)gpu_registers[RB_HIZCONTROL], (unsigned)gpu_registers[RB_HIZ_CLEAR],
		(unsigned)gpu_registers[RB_DEPTHCONTROL], (unsigned)gpu_registers[RB_SURFACE_INFO],
		(unsigned)gpu_registers[RB_DEPTH_INFO], (unsigned)gpu_registers[RB_DEPTH_CLEAR],
		(unsigned)gpu_registers[RB_COPY_CONTROL]);
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
	xsdk_log_write("hiz probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("hiz probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	log_registers();
	if (!set_up()) {
		xsdk_log_write("hiz probe: set-up failed");
		return 1;
	}
	xsdk_gpu_set_depth_clear(1.0f, 0);
	begin_target();
	// eDRAM holds whatever was there; this resolve clears it, so the first check is of the clear alone.
	run_phase("first clear", NULL, 0, NO_LADDER, &PLAIN);
	ladder("start, RB_HIZCONTROL 0", 0);
	ladder("start, RB_HIZCONTROL 2", 2);
	ladder("start, RB_HIZCONTROL 3", 3);
	xenia_bits();
	clears();
	kills();
	farther_writes();
	greater();
	single_bits();
	xsdk_log_write("hiz probe: %s", failed ? "stopped early" : "done");
	xsdk_log_write("hiz probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
