#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "probe_gpu.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// What vulkan4xenon's queries and events need of the command processor (vulkan4xenon:docs/ROADMAP.md, milestone 19's
// step 6): whether the clock's high word counts; whether the command processor waits on a word in memory until the CPU
// writes it; the sample counts ZPASS_DONE writes, and whether they've landed once a later event write has. As it
// stands, the sixth run's: Adreno 3xx's sequence with PM4_EVENT_WRITE. Each run's change is in docs/HARDWARE.md,
// "Queries and events", and its log in results/2026-10-16-queries-probe/; ZPASS_DONE hung the GPU in all six.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	FENCE_TIMEOUT_US = 2000000,
	CLEAR_ARGB = 0xFF202020,
	QUAD_ARGB = 0xFF40E040,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// Each quad covers 256x128 pixels.
	QUAD_WIDTH = 256,
	QUAD_HEIGHT = 128,
	QUAD_PIXELS = QUAD_WIDTH * QUAD_HEIGHT,
	// Registers by index (xenia:src/xenia/gpu/register_table.inc:42-52, 607-608).
	CP_PERFMON_CNTL = 0x01F5,
	RBBM_PERFCOUNTER1_SELECT = 0x0396,
	RBBM_PERFCOUNTER1_LOW = 0x0399,
	RBBM_PERFCOUNTER1_HI = 0x039A,
	RB_SAMPLE_COUNT_CTL = 0x2324,
	RB_SAMPLE_COUNT_ADDR = 0x2325,
	// Type-3 opcodes and events (xenia:src/xenia/gpu/xenos.h:1541-1622).
	PM4_WAIT_REG_MEM = 0x3C,
	PM4_EVENT_WRITE = 0x46,
	PM4_EVENT_WRITE_SHD = 0x58,
	PM4_EVENT_WRITE_ZPD = 0x5B,
	PS_DONE_TS = 3,
	ZPASS_DONE = 21,
	// RB_SAMPLE_COUNT_CONTROL's COPY on Adreno 3xx (Linux:drivers/gpu/drm/msm/registers/adreno/a3xx.xml), and the
	// value the console leaves RB_SAMPLE_COUNT_CTL at (results/2026-10-13-milestone-31/probe_pixel_rate.log).
	SAMPLE_COUNT_COPY = 2,
	SWAP_8IN32 = 2,
	// WAIT_REG_MEM's equal function and its memory bit (xenia:src/xenia/gpu/command_processor.cc:955-1000), at xsdk's
	// interval (xsdk:src/gpu_setup.c).
	WAIT_EQUAL = 3,
	WAIT_MEMORY = 0x10,
	WAIT_INTERVAL = 8,
	// The counters count every clock once on (docs/HARDWARE.md, "A clock the GPU and the CPU both read").
	PERF_STATE_RESET = 0,
	PERF_STATE_ENABLE = 1,
	RBBM1_COUNT = 0,
	// The counts are eight words (xenia:src/xenia/gpu/xenos.h:1525-1537): Total, ZFail, ZPass and StencilFail, each
	// as A and B.
	COUNT_WORDS = 8,
	COUNT_FIELDS = 4,
	SLOT_BYTES = 256,
	SLOTS = 4,
	// D3D's mark of counts not yet written (xenia:src/xenia/gpu/command_processor.cc:1252-1270).
	UNWRITTEN = 0xFFFFFEED,
	ORDER_ROUNDS = 20,
	ORDER_DRAWS = 10,
	// The flag word and the waited-on word, after the slots.
	FLAG_WORD = SLOTS * SLOT_BYTES / 4,
	WAITED_WORD = FLAG_WORD + 1,
	MEMORY_BYTES = SLOTS * SLOT_BYTES + 16,
	WAIT_BEFORE_WRITE_MS = 50,
	WAIT_LONG_MS = 2000,
	WAIT_AFTER_WRITE_US = 100000,
	NEAR_WRAP = 0xFFFFF000,
	// The clock's 498.9 a microsecond (docs/HARDWARE.md), so the low word wraps within 8.61 s.
	CLOCKS_PER_US = 498,
	WRAP_MARGIN_MS = 200,
};

static const char *const field_names[COUNT_FIELDS] = { "total", "z fail", "z pass", "stencil fail" };

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

static XsdkShader vertex_shader;
static XsdkShader pixel_shader;
static uint32_t vertices_address;
static volatile uint32_t *target;
static volatile uint32_t *memory;
static uint32_t memory_address;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static void sleep_ms(uint32_t ms)
{
	const struct timespec request = { .tv_sec = ms / 1000, .tv_nsec = (long)(ms % 1000) * 1000000 };
	(void)nanosleep(&request, NULL);
}

static bool fence_landed(uint32_t fence)
{
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("queries probe: fence %u didn't land", (unsigned)fence);
			return false;
		}
	}
	return true;
}

static void put_register(uint32_t index, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = index;
	packet[1] = value;
}

// The sample counts into a slot, as Adreno 3xx's are copied: the address, the control's copy bit, a draw of no points,
// then the event (freedreno:fdre-a3xx/freedreno.c:1113-1127, from a capture of Qualcomm's driver). The event alone
// hung the draw after it, whatever the control (results/2026-10-16-queries-probe/).
static void put_counts(uint32_t slot)
{
	put_register(RB_SAMPLE_COUNT_ADDR, memory_address + slot * SLOT_BYTES);
	put_register(RB_SAMPLE_COUNT_CTL, SAMPLE_COUNT_COPY);
	xsdk_gpu_draw(XSDK_PRIMITIVE_POINT_LIST, 0);
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE, 1);
	packet[1] = ZPASS_DONE;
}

static void put_event_write(uint32_t word, uint32_t value)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_SHD, 3);
	packet[1] = PS_DONE_TS;
	packet[2] = (memory_address + word * 4) | SWAP_8IN32;
	packet[3] = value;
}

static void mark_unwritten(void)
{
	for (uint32_t i = 0; i < SLOTS * SLOT_BYTES / 4; i++)
		memory[i] = UNWRITTEN;
	__asm__ volatile("sync" ::: "memory");
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The unit square scaled onto QUAD_WIDTH by QUAD_HEIGHT pixels from (x, y), as the full screen is drawn
// (tests/probe_depth_export.c).
static void draw_quad(uint32_t x, uint32_t y, float depth)
{
	const float vertex[3][4] = {
		{ 2.0f * QUAD_WIDTH / WIDTH, -2.0f * QUAD_HEIGHT / HEIGHT, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH + 2.0f * x / WIDTH, 1.0f + 1.0f / HEIGHT - 2.0f * y / HEIGHT, depth, 0.0f },
		{ unorm(QUAD_ARGB >> 16), unorm(QUAD_ARGB >> 8), unorm(QUAD_ARGB), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// A resolve clearing colour and depth to 1, then the target over it, depth less-than and written.
static bool begin(void)
{
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(target), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!fence_landed(xsdk_gpu_fence()))
		return false;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
	return true;
}

static uint32_t field(uint32_t slot, uint32_t index, bool swapped)
{
	const uint32_t a = memory[slot * SLOT_BYTES / 4 + index * 2];
	const uint32_t b = memory[slot * SLOT_BYTES / 4 + index * 2 + 1];
	return swapped ? __builtin_bswap32(a) + __builtin_bswap32(b) : a + b;
}

static void log_slot(uint32_t slot)
{
	volatile const uint32_t *const words = &memory[slot * SLOT_BYTES / 4];
	xsdk_log_write("queries probe: slot %u: %08X %08X %08X %08X %08X %08X %08X %08X", (unsigned)slot,
		(unsigned)words[0], (unsigned)words[1], (unsigned)words[2], (unsigned)words[3], (unsigned)words[4],
		(unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
}

static void log_counts(void)
{
	for (uint32_t slot = 0; slot < SLOTS; slot++)
		log_slot(slot);
	for (uint32_t slot = 1; slot < SLOTS; slot++) {
		for (uint32_t index = 0; index < COUNT_FIELDS; index++) {
			xsdk_log_write("queries probe: slots %u to %u: %s A+B, swapped, %d; as stored, %d", (unsigned)slot - 1,
				(unsigned)slot, field_names[index], (int)(field(slot, index, true) - field(slot - 1, index, true)),
				(int)(field(slot, index, false) - field(slot - 1, index, false)));
		}
	}
}

// A fence after a packet, to show which one the GPU hangs on, and where the command processor stands if it does.
static bool step(const char *what)
{
	const bool landed = fence_landed(xsdk_gpu_fence());
	xsdk_log_write("queries probe: %s: %s", what, landed ? "landed" : "hung");
	if (!landed)
		probe_report_cp("queries probe", what);
	return landed;
}

// The sequence a step at a time, a quad between, as the event alone hung the next draw.
static bool probe_sequence(void)
{
	if (!begin())
		return false;
	mark_unwritten();
	put_counts(0);
	if (!step("the counts, nothing drawn"))
		return false;
	log_slot(0);
	draw_quad(0, 0, 0.5f);
	if (!step("a quad after them"))
		return false;
	put_counts(1);
	if (!step("the counts after the quad"))
		return false;
	draw_quad(512, 256, 0.25f);
	if (!step("a quad after those"))
		return false;
	log_counts();
	return true;
}

// Counts before a quad that passes, after it, after one behind it that fails, and after two that pass.
static bool probe_counts(void)
{
	if (!begin())
		return false;
	mark_unwritten();
	put_counts(0);
	draw_quad(0, 0, 0.5f);
	put_counts(1);
	draw_quad(0, 0, 0.75f);
	put_counts(2);
	draw_quad(512, 256, 0.25f);
	draw_quad(0, 0, 0.25f);
	put_counts(3);
	if (!fence_landed(xsdk_gpu_fence()))
		return false;
	xsdk_log_write("queries probe: one quad passing, one failing, two passing, %u pixels each", (unsigned)QUAD_PIXELS);
	log_counts();
	return true;
}

// Whether the counts have landed once an event write after them has, which the CPU spins on.
static bool probe_order(void)
{
	if (!begin())
		return false;
	uint32_t landed = 0;
	for (uint32_t round = 1; round <= ORDER_ROUNDS; round++) {
		mark_unwritten();
		for (uint32_t i = 0; i < ORDER_DRAWS; i++)
			draw_quad(0, 0, 0.5f - 0.01f * (float)(round * ORDER_DRAWS + i) / (ORDER_ROUNDS * ORDER_DRAWS));
		put_counts(0);
		put_event_write(FLAG_WORD, round);
		const uint32_t fence = xsdk_gpu_fence();
		const uint64_t start = now_us();
		while (memory[FLAG_WORD] != round && now_us() - start < FENCE_TIMEOUT_US) {
		}
		// ZPass_A, the third field's first word.
		landed += memory[2 * 2] != UNWRITTEN;
		if (!fence_landed(fence))
			return false;
	}
	xsdk_log_write("queries probe: %u of %u counts had landed when the event write after them had", (unsigned)landed,
		(unsigned)ORDER_ROUNDS);
	return true;
}

static void log_clock(const char *when)
{
	xsdk_log_write("queries probe: clock %s: high %08X, low %08X", when,
		(unsigned)probe_gpu_registers[RBBM_PERFCOUNTER1_HI], (unsigned)probe_gpu_registers[RBBM_PERFCOUNTER1_LOW]);
}

// The low word set near its wrap, then, if that doesn't hold, left to wrap by itself.
static void probe_clock(void)
{
	volatile uint32_t *const registers = probe_gpu_registers;
	registers[CP_PERFMON_CNTL] = PERF_STATE_RESET;
	registers[RBBM_PERFCOUNTER1_SELECT] = RBBM1_COUNT;
	registers[CP_PERFMON_CNTL] = PERF_STATE_ENABLE;
	log_clock("on");
	registers[RBBM_PERFCOUNTER1_LOW] = NEAR_WRAP;
	log_clock("low set near its wrap");
	sleep_ms(1);
	log_clock("1 ms on");
	const uint32_t low = registers[RBBM_PERFCOUNTER1_LOW];
	const uint32_t wrap_ms = (uint32_t)((0xFFFFFFFFu - low) / CLOCKS_PER_US / 1000) + WRAP_MARGIN_MS;
	xsdk_log_write("queries probe: clock: waiting %u ms for the low word to wrap", (unsigned)wrap_ms);
	sleep_ms(wrap_ms);
	log_clock("past the wrap");
}

// The command processor waits on the word for the CPU's 1, read through the swap given, or not; a fence after it.
static bool probe_wait(uint32_t swap, uint32_t before_ms)
{
	memory[WAITED_WORD] = 0;
	__asm__ volatile("sync" ::: "memory");
	volatile uint32_t *const packet = xsdk_gpu_reserve(6);
	packet[0] = xsdk_gpu_type3(PM4_WAIT_REG_MEM, 5);
	packet[1] = WAIT_MEMORY | WAIT_EQUAL;
	packet[2] = (memory_address + WAITED_WORD * 4) | swap;
	packet[3] = 1;
	packet[4] = 0xFFFFFFFF;
	packet[5] = WAIT_INTERVAL;
	const uint32_t fence = xsdk_gpu_fence();
	sleep_ms(before_ms);
	const bool early = xsdk_gpu_fence_done(fence);
	const uint64_t written = now_us();
	memory[WAITED_WORD] = 1;
	__asm__ volatile("sync" ::: "memory");
	while (!xsdk_gpu_fence_done(fence) && now_us() - written < WAIT_AFTER_WRITE_US) {
	}
	const uint64_t after_us = now_us() - written;
	const bool passed = xsdk_gpu_fence_done(fence);
	if (!passed) {
		// What the command processor reads without the swap.
		memory[WAITED_WORD] = 0x01000000;
		__asm__ volatile("sync" ::: "memory");
	}
	xsdk_log_write("queries probe: wait, swap %u, %u ms before the write: passed before it %s; after it %s, %llu us",
		(unsigned)swap, (unsigned)before_ms, early ? "yes" : "no", passed ? "yes" : "no", (unsigned long long)after_us);
	return fence_landed(fence);
}

static bool set_up(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, constant_vertex_shader, sizeof constant_vertex_shader)) ||
		!NT_SUCCESS(xsdk_shader_load(&pixel_shader, triangle_pixel_shader, sizeof triangle_pixel_shader)) ||
		!NT_SUCCESS(xsdk_shader_set_vertex_layout(&vertex_shader, layout, 1, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	volatile uint32_t *const vertices = xsdk_gpu_alloc(sizeof square);
	target = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	memory = xsdk_gpu_alloc(MEMORY_BYTES);
	if (!vertices || !target || !memory)
		return false;
	memory_address = xsdk_gpu_address(memory);
	uint32_t words[sizeof square / 4];
	memcpy(words, square, sizeof square);
	for (uint32_t i = 0; i < sizeof square / 4; i++)
		vertices[i] = words[i];
	__asm__ volatile("sync" ::: "memory");
	vertices_address = xsdk_gpu_address(vertices);
	xsdk_gpu_invalidate(vertices_address, sizeof square);
	return true;
}

int main(void)
{
	xsdk_log_write("queries probe");
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("queries probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("queries probe: set-up failed");
		return 1;
	}
	// What hangs the GPU goes last: the event did, on the first run (results/2026-10-16-queries-probe/).
	probe_clock();
	bool landed = probe_wait(SWAP_8IN32, WAIT_BEFORE_WRITE_MS) && probe_wait(0, WAIT_BEFORE_WRITE_MS) &&
		probe_wait(SWAP_8IN32, WAIT_LONG_MS);
	const uint32_t found = probe_gpu_registers[RB_SAMPLE_COUNT_CTL];
	xsdk_log_write("queries probe: RB_SAMPLE_COUNT_CTL as found: %08X", (unsigned)found);
	landed = landed && probe_sequence() && probe_counts() && probe_order();
	if (landed)
		put_register(RB_SAMPLE_COUNT_CTL, found);
	xsdk_log_write("queries probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
