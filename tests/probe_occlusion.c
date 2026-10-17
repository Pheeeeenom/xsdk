#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "constant_shaders.h"
#include "probe_gpu.h"
#include "tiled_index.h"
#include "triangle_shaders.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"

// vulkan4xenon milestone 28's tries at the sample counts ZPASS_DONE never wrote in probe_queries.c's six runs, where it
// hung the GPU each time (docs/HARDWARE.md, "Queries and events"). A build runs the try OCCLUSION_TRY names, from the
// table below; results/2026-10-16-vulkan4xenon-milestone-28/builds.sh builds each into probe_occlusion_<try>.xex.
// Every try but registers sends what the console hasn't run and can hang the GPU. Each wait is bounded, so one that
// hangs still logs the step it hung at, the registers and a last line, but the GPU stays hung once the title exits.
#ifndef OCCLUSION_TRY
#define OCCLUSION_TRY "registers"
#endif

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	FENCE_TIMEOUT_US = 2000000,
	CLEAR_ARGB = 0xFF202020,
	QUAD_ARGB = 0xFF40E040,
	HIDDEN_ARGB = 0xFFE04040,
	CHANNEL_TOLERANCE = 1,
	QUAD_VERTICES = 6,
	VERTEX_FLOATS = 4,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	// Each quad covers 256x128 pixels.
	QUAD_WIDTH = 256,
	QUAD_HEIGHT = 128,
	QUAD_PIXELS = QUAD_WIDTH * QUAD_HEIGHT,
	// Type-3 opcodes and events (xenia:src/xenia/gpu/xenos.h:1548-1559, 1588-1605).
	PM4_VIZ_QUERY = 0x23,
	PM4_REG_TO_MEM = 0x3E,
	PM4_EVENT_WRITE = 0x46,
	PM4_EVENT_WRITE_ZPD = 0x5B,
	CACHE_FLUSH = 6,
	RST_PIX_CNT = 13,
	ZPASS_DONE = 21,
	// RB_SAMPLE_COUNT_CTL's bits (amd-gpu:include/reg/yamato/22/yamato_mask.h:5578-5581), and the counters' off switch
	// in RB_BC_CONTROL (5605; Linux:drivers/gpu/drm/msm/registers/adreno/a2xx.xml:1280).
	SAMPLE_COUNT_RESET = 1,
	SAMPLE_COUNT_COPY = 2,
	DISABLE_SAMPLE_COUNTERS = 1 << 16,
	// PA_SC_VIZ_QUERY's fields (xenia:src/xenia/gpu/registers.h:517-531), and VIZ_QUERY's end of a query
	// (command_processor.cc:1601-1633).
	VIZ_QUERY_ENA = 1,
	VIZ_QUERY_ID_SHIFT = 1,
	KILL_PIX_POST_HI_Z = 1 << 7,
	VIZ_QUERY_END = 0x100,
	// As xsdk waits for the GPU to be idle (src/gpu_private.h).
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	SWAP_8IN32 = 2,
	// The counts are eight words (xenia:src/xenia/gpu/xenos.h:1525-1537): Total, ZFail, ZPass and StencilFail, each
	// as A and B.
	COUNT_WORDS = 8,
	COUNT_FIELDS = 4,
	SLOT_WORDS = 64,
	// The counting's three slots, then one for a lone event.
	SLOTS = 4,
	LONE_SLOT = 3,
	// Where registers are copied to, after the slots.
	COPY_WORD = SLOTS * SLOT_WORDS,
	COPY_WORDS = 32,
	MEMORY_BYTES = (COPY_WORD + COPY_WORDS) * 4,
	// D3D's mark of counts not yet written (xenia:src/xenia/gpu/command_processor.cc:1253-1273).
	UNWRITTEN = 0xFFFFFEED,
	// The raster group's 19 registers, the most of any group, at 23 characters each in the log.
	GROUP_MAX_REGISTERS = 19,
	GROUP_LINE_BYTES = 448,
	STEP_NAME_BYTES = 96,
	// What xsdk's set-up alone writes of the drawing registers it changes (src/gpu_setup.c, put_draw_state and
	// put_state): the rest a resolve and the draw state write again.
	SET_UP_POINT_SIZE = 0x00080008,
	SET_UP_PIX_CENTER_D3D = 4,
	SET_UP_SCREEN_SCISSOR_BR = 8192 | 8192 << 16,
	SET_UP_VS_CONST = 0 | 255 << 12,
	SET_UP_PS_CONST = 256 | 255 << 12,
};

// Registers by index (xenia:src/xenia/gpu/register_table.inc), the unknown ones named in Xenia's style
// (src/gpu_private.h).
enum {
	UNKNOWN_0081 = 0x0081,
	UNKNOWN_0082 = 0x0082,
	CP_PERFMON_CNTL = 0x01F5,
	RBBM_PERFCOUNTER1_SELECT = 0x0396,
	COHER_STATUS_HOST = 0x0A31,
	PA_SC_VIZ_QUERY_STATUS_0 = 0x0C44,
	PA_SC_VIZ_QUERY_STATUS_1 = 0x0C45,
	PA_CL_ENHANCE = 0x0C85,
	SQ_GPR_MANAGEMENT = 0x0D00,
	SQ_FLOW_CONTROL = 0x0D01,
	SQ_INST_STORE_MANAGMENT = 0x0D02,
	SQ_EO_RT = 0x0D04,
	UNKNOWN_0E42 = 0x0E42,
	RB_BC_CONTROL = 0x0F01,
	RB_EDRAM_INFO = 0x0F02,
	// Yamato's sample counters, which Xenia doesn't name: it has BC_PERFCOUNTER3_HI at the first
	// (amd-gpu:include/reg/yamato/22/yamato_offset.h:578-581; xenia:src/xenia/gpu/register_table.inc:276).
	RB_TOTAL_SAMPLES = 0x0F0F,
	RB_ZPASS_SAMPLES = 0x0F10,
	RB_ZFAIL_SAMPLES = 0x0F11,
	RB_SFAIL_SAMPLES = 0x0F12,
	RB_SURFACE_INFO = 0x2000,
	RB_COLOR_INFO = 0x2001,
	RB_DEPTH_INFO = 0x2002,
	COHER_DEST_BASE_7 = 0x200D,
	PA_SC_SCREEN_SCISSOR_TL = 0x200E,
	PA_SC_SCREEN_SCISSOR_BR = 0x200F,
	PA_SC_WINDOW_OFFSET = 0x2080,
	PA_SC_WINDOW_SCISSOR_TL = 0x2081,
	PA_SC_WINDOW_SCISSOR_BR = 0x2082,
	VGT_MAX_VTX_INDX = 0x2100,
	VGT_MIN_VTX_INDX = 0x2101,
	VGT_INDX_OFFSET = 0x2102,
	RB_COLOR_MASK = 0x2104,
	PA_CL_VPORT_XSCALE = 0x210F,
	PA_CL_VPORT_XOFFSET = 0x2110,
	PA_CL_VPORT_YSCALE = 0x2111,
	PA_CL_VPORT_YOFFSET = 0x2112,
	PA_CL_VPORT_ZSCALE = 0x2113,
	PA_CL_VPORT_ZOFFSET = 0x2114,
	SQ_PROGRAM_CNTL = 0x2180,
	SQ_CONTEXT_MISC = 0x2181,
	RB_DEPTHCONTROL = 0x2200,
	RB_HIZCONTROL = 0x2203,
	PA_CL_CLIP_CNTL = 0x2204,
	PA_SU_SC_MODE_CNTL = 0x2205,
	PA_CL_VTE_CNTL = 0x2206,
	RB_MODECONTROL = 0x2208,
	PA_SU_POINT_SIZE = 0x2280,
	PA_SC_VIZ_QUERY = 0x2293,
	PA_SC_AA_CONFIG = 0x2301,
	PA_SU_VTX_CNTL = 0x2302,
	SQ_VS_CONST = 0x2307,
	SQ_PS_CONST = 0x2308,
	PA_SC_AA_MASK = 0x2312,
	RB_COPY_CONTROL = 0x2318,
	RB_COPY_DEST_BASE = 0x2319,
	RB_COPY_DEST_PITCH = 0x231A,
	RB_COPY_DEST_INFO = 0x231B,
	RB_HIZ_CLEAR = 0x231C,
	RB_DEPTH_CLEAR = 0x231D,
	RB_COLOR_CLEAR = 0x231E,
	RB_COLOR_CLEAR_LO = 0x231F,
	RB_SAMPLE_COUNT_CTL = 0x2324,
	RB_SAMPLE_COUNT_ADDR = 0x2325,
};

enum {
	GROUP_CLOCK,
	GROUP_SHADER_UNIT,
	GROUP_BACKEND,
	GROUP_SURFACE,
	GROUP_COPY,
	GROUP_RASTER,
	GROUP_SHADER_STATE,
	GROUPS,
	ALL_GROUPS = (1 << GROUPS) - 1,
	// The groups below 0x2000, which read back what the set-up wrote but SQ_FLOW_CONTROL; drawing registers read the
	// same before and after it (docs/HARDWARE.md, "libxenon's set-up makes the resolves work").
	CONTROL_GROUPS = 1 << GROUP_CLOCK | 1 << GROUP_SHADER_UNIT | 1 << GROUP_BACKEND,
};

typedef struct {
	const char *name;
	const uint16_t *indices;
	uint32_t count;
} RegisterGroup;

// src/gpu_setup.c's saved_registers, by the step that writes them: put_clock; put_shader_unit and put_register_split;
// put_render_backend; then the drawing state of put_draw_state, put_state and the resolves.
static const uint16_t clock_registers[] = { CP_PERFMON_CNTL, RBBM_PERFCOUNTER1_SELECT };
static const uint16_t shader_unit_registers[] = { UNKNOWN_0081, UNKNOWN_0082, SQ_GPR_MANAGEMENT, SQ_FLOW_CONTROL,
	SQ_INST_STORE_MANAGMENT, SQ_EO_RT, UNKNOWN_0E42, PA_CL_ENHANCE };
static const uint16_t backend_registers[] = { RB_BC_CONTROL };
static const uint16_t surface_registers[] = { RB_SURFACE_INFO, RB_COLOR_INFO, RB_DEPTH_INFO, COHER_DEST_BASE_7,
	RB_COLOR_MASK, RB_DEPTHCONTROL, RB_HIZCONTROL, RB_MODECONTROL, PA_SC_AA_CONFIG, PA_SC_AA_MASK };
static const uint16_t copy_registers[] = { RB_COPY_CONTROL, RB_COPY_DEST_BASE, RB_COPY_DEST_PITCH, RB_COPY_DEST_INFO,
	RB_HIZ_CLEAR, RB_DEPTH_CLEAR, RB_COLOR_CLEAR, RB_COLOR_CLEAR_LO };
static const uint16_t raster_registers[] = { PA_SC_SCREEN_SCISSOR_TL, PA_SC_SCREEN_SCISSOR_BR, PA_SC_WINDOW_OFFSET,
	PA_SC_WINDOW_SCISSOR_TL, PA_SC_WINDOW_SCISSOR_BR, VGT_MAX_VTX_INDX, VGT_MIN_VTX_INDX, VGT_INDX_OFFSET,
	PA_CL_VPORT_XSCALE, PA_CL_VPORT_XOFFSET, PA_CL_VPORT_YSCALE, PA_CL_VPORT_YOFFSET, PA_CL_VPORT_ZSCALE,
	PA_CL_VPORT_ZOFFSET, PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, PA_SU_POINT_SIZE, PA_SU_VTX_CNTL };
static const uint16_t shader_state_registers[] = { SQ_PROGRAM_CNTL, SQ_CONTEXT_MISC, SQ_VS_CONST, SQ_PS_CONST };

static const RegisterGroup groups[GROUPS] = {
	{ "clock", clock_registers, sizeof clock_registers / sizeof clock_registers[0] },
	{ "shader unit", shader_unit_registers, sizeof shader_unit_registers / sizeof shader_unit_registers[0] },
	{ "render backend", backend_registers, sizeof backend_registers / sizeof backend_registers[0] },
	{ "surface", surface_registers, sizeof surface_registers / sizeof surface_registers[0] },
	{ "copy", copy_registers, sizeof copy_registers / sizeof copy_registers[0] },
	{ "raster", raster_registers, sizeof raster_registers / sizeof raster_registers[0] },
	{ "shader state", shader_state_registers, sizeof shader_state_registers / sizeof shader_state_registers[0] },
};

// A try sends ZPASS_DONE unless its kind is one of the others.
enum {
	KIND_ZPASS,
	KIND_REGISTERS,
	KIND_VIZ_QUERY,
};

// What a ZPASS_DONE try sends once before the counting, and after each event.
enum {
	FIRST_NONE,
	FIRST_RESET,
	FIRST_RST_PIX_CNT,
};

enum {
	AFTER_NONE,
	AFTER_FLUSH,
	AFTER_RESOLVE,
};

typedef struct {
	const char *name;
	uint32_t kind;
	uint32_t first;
	// RB_SAMPLE_COUNT_CTL at each event.
	uint32_t control;
	uint32_t after;
	// Put back as the console left them before each event, and as xsdk set them after it.
	uint32_t groups;
	// RB_BC_CONTROL's DISABLE_SAMPLE_COUNTERS set throughout the counting.
	bool counters_off;
} Try;

// Each ZPASS_DONE try sends it as D3D does (xenia:src/xenia/gpu/command_processor.cc:1249-1281) after a clear, a quad
// and a quad hidden behind it.
static const Try tries[] = {
	// Yamato's counters as registers, without ZPASS_DONE (amd-gpu:include/reg/yamato/22/yamato_offset.h:578-581).
	{ .name = "registers", .kind = KIND_REGISTERS },
	// D3D's visibility query, answered in a register (xenia:src/xenia/gpu/command_processor.cc:1601-1633).
	{ .name = "viz_query", .kind = KIND_VIZ_QUERY },
	// RESET alone at a first event, then COPY at each.
	{ .name = "reset", .first = FIRST_RESET, .control = SAMPLE_COUNT_COPY },
	{ .name = "reset_copy", .control = SAMPLE_COUNT_RESET | SAMPLE_COUNT_COPY },
	// The event resetting the pixel counts, first (Linux:drivers/gpu/drm/msm/registers/adreno/adreno_pm4.xml:28).
	{ .name = "reset_event", .first = FIRST_RST_PIX_CNT, .control = SAMPLE_COUNT_COPY },
	// After each event, the cache flush a resolve sends after its copy (libxenon:drivers/xenos/xe.c:1151).
	{ .name = "flush", .control = SAMPLE_COUNT_COPY, .after = AFTER_FLUSH },
	// A resolve between each event and the next draw.
	{ .name = "resolve", .control = SAMPLE_COUNT_COPY, .after = AFTER_RESOLVE },
	// Whether the event hangs with the counters off.
	{ .name = "no_counters", .control = SAMPLE_COUNT_COPY, .counters_off = true },
	// The state the console leaves before xsdk's set-up, all of it, then a group at a time.
	{ .name = "group_all", .control = SAMPLE_COUNT_COPY, .groups = ALL_GROUPS },
	{ .name = "group_clock", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_CLOCK },
	{ .name = "group_shader_unit", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_SHADER_UNIT },
	{ .name = "group_backend", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_BACKEND },
	{ .name = "group_surface", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_SURFACE },
	{ .name = "group_copy", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_COPY },
	{ .name = "group_raster", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_RASTER },
	{ .name = "group_shader_state", .control = SAMPLE_COUNT_COPY, .groups = 1 << GROUP_SHADER_STATE },
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
// As read before xsdk_gpu_open, and once it has set the GPU up.
static uint32_t found[GROUPS][GROUP_MAX_REGISTERS];
static uint32_t opened[GROUPS][GROUP_MAX_REGISTERS];
// For the last line: where the try stopped, and how many of its events wrote their counts.
static char stopped_at[STEP_NAME_BYTES] = "finished";
static uint32_t events_sent;
static uint32_t events_written;

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

static bool fence_landed(uint32_t fence)
{
	const uint64_t start = now_us();
	while (!xsdk_gpu_fence_done(fence)) {
		if (now_us() - start > FENCE_TIMEOUT_US) {
			xsdk_log_write("occlusion probe: fence %u didn't land", (unsigned)fence);
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

static void put_event(uint32_t event)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE, 1);
	packet[1] = event;
}

static void put_wait_idle(void)
{
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
}

// A register into a word of memory through the swap, so the CPU reads what it holds, as xsdk_gpu_write_clock copies
// the clock (src/gpu.c).
static void put_copy(uint32_t index, uint32_t word)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(3);
	packet[0] = xsdk_gpu_type3(PM4_REG_TO_MEM, 2);
	packet[1] = index;
	packet[2] = (memory_address + word * 4) | SWAP_8IN32;
}

// Where the command processor stands, as probe_queries.c logs it, and the render backend's registers Xenia names.
static void report(const char *when)
{
	volatile const uint32_t *const registers = probe_gpu_registers;
	probe_report_cp("occlusion probe", when);
	xsdk_log_write(
		"occlusion probe: %s: RB_BC_CONTROL %08X, RB_EDRAM_INFO %08X, COHER_STATUS_HOST %08X, "
		"RB_SAMPLE_COUNT_CTL %08X, viz status %08X %08X",
		when, (unsigned)registers[RB_BC_CONTROL], (unsigned)registers[RB_EDRAM_INFO],
		(unsigned)registers[COHER_STATUS_HOST], (unsigned)registers[RB_SAMPLE_COUNT_CTL],
		(unsigned)registers[PA_SC_VIZ_QUERY_STATUS_0], (unsigned)registers[PA_SC_VIZ_QUERY_STATUS_1]);
}

// A fence after what's queued, once the GPU is idle if asked; where one doesn't land, the try stops.
static bool step(const char *what, bool idle)
{
	if (idle)
		put_wait_idle();
	const bool landed = fence_landed(xsdk_gpu_fence());
	const char *const stage = idle ? ", idle" : "";
	xsdk_log_write("occlusion probe: %s%s: %s", what, stage, landed ? "landed" : "hung");
	if (!landed) {
		(void)snprintf(stopped_at, sizeof stopped_at, "hung at %s%s", what, stage);
		report(what);
	}
	return landed;
}

static void mark_unwritten(void)
{
	for (uint32_t i = 0; i < MEMORY_BYTES / 4; i++)
		memory[i] = UNWRITTEN;
	__asm__ volatile("sync" ::: "memory");
}

static bool slot_written(uint32_t slot)
{
	for (uint32_t i = 0; i < COUNT_WORDS; i++) {
		if (memory[slot * SLOT_WORDS + i] != UNWRITTEN)
			return true;
	}
	return false;
}

static void log_slot(uint32_t slot, const char *what)
{
	volatile const uint32_t *const words = &memory[slot * SLOT_WORDS];
	xsdk_log_write("occlusion probe: %s: slot %u %s: %08X %08X %08X %08X %08X %08X %08X %08X", what, (unsigned)slot,
		slot_written(slot) ? "written" : "unwritten", (unsigned)words[0], (unsigned)words[1], (unsigned)words[2],
		(unsigned)words[3], (unsigned)words[4], (unsigned)words[5], (unsigned)words[6], (unsigned)words[7]);
}

// A field's A and B summed, as D3D sums them, read little-endian as Xenia reads them, or as stored.
static uint32_t field(uint32_t slot, uint32_t index, bool swapped)
{
	const uint32_t a = memory[slot * SLOT_WORDS + index * 2];
	const uint32_t b = memory[slot * SLOT_WORDS + index * 2 + 1];
	return swapped ? __builtin_bswap32(a) + __builtin_bswap32(b) : a + b;
}

static int32_t field_change(uint32_t slot, uint32_t index, bool swapped)
{
	return (int32_t)(field(slot + 1, index, swapped) - field(slot, index, swapped));
}

static void log_interval(uint32_t slot, const char *what)
{
	for (uint32_t index = 0; index < COUNT_FIELDS; index++) {
		xsdk_log_write("occlusion probe: slots %u to %u, %s: %s A+B, swapped, %d; as stored, %d", (unsigned)slot,
			(unsigned)slot + 1, what, field_names[index], (int)field_change(slot, index, true),
			(int)field_change(slot, index, false));
	}
}

static float unorm(uint32_t value)
{
	return (value & 0xFF) / 255.0f;
}

// The unit square scaled onto QUAD_WIDTH by QUAD_HEIGHT pixels from (x, y), as the full screen is drawn
// (tests/probe_depth_export.c).
static void draw_quad(uint32_t x, uint32_t y, float depth, uint32_t argb)
{
	const float vertex[3][4] = {
		{ 2.0f * QUAD_WIDTH / WIDTH, -2.0f * QUAD_HEIGHT / HEIGHT, 1.0f, 1.0f },
		{ -1.0f - 1.0f / WIDTH + 2.0f * x / WIDTH, 1.0f + 1.0f / HEIGHT - 2.0f * y / HEIGHT, depth, 0.0f },
		{ unorm(argb >> 16), unorm(argb >> 8), unorm(argb), 1.0f },
	};
	xsdk_gpu_set_vertex_constants(0, &vertex[0][0], 3);
	xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
}

// The target, the quads' vertices and shaders, and depth less-than and written, as a resolve leaves them to be set.
static void set_state(void)
{
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_vertices(0, vertices_address, sizeof square);
	xsdk_gpu_set_shaders(&vertex_shader, &pixel_shader);
	const XsdkDepthStencil setting = { .depth_test = true, .depth_write = true, .depth_compare = XSDK_COMPARE_LESS };
	XsdkDepthStencilState state;
	xsdk_gpu_depth_stencil_state(&state, &setting);
	xsdk_gpu_set_depth_stencil(&state);
}

// A resolve clearing colour and depth to 1, then the state over it.
static bool begin(void)
{
	xsdk_gpu_set_depth_clear(1.0f, 0);
	xsdk_gpu_resolve(xsdk_gpu_address(target), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!step("the clear", false))
		return false;
	set_state();
	return true;
}

// A resolve that clears nothing, so a quad after it is tested against the depth before it.
static void copy_out(void)
{
	xsdk_gpu_set_resolve_clears(false, false);
	xsdk_gpu_resolve(xsdk_gpu_address(target), WIDTH, HEIGHT, CLEAR_ARGB);
	xsdk_gpu_set_resolve_clears(true, true);
	set_state();
}

static bool near(uint32_t got, uint32_t wanted)
{
	return (got > wanted ? got - wanted : wanted - got) <= CHANNEL_TOLERANCE;
}

static bool same_colour(uint32_t stored, uint32_t want)
{
	return near(stored >> 8 & 0xFF, want >> 8 & 0xFF) && near(stored >> 16 & 0xFF, want >> 16 & 0xFF) &&
		near(stored >> 24, want >> 24);
}

// A resolve, then the pixels of each quad's colour in it (tests/probe_depth_export.c).
static bool count_pixels(const char *what)
{
	xsdk_gpu_resolve(xsdk_gpu_address(target), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!step(what, false))
		return false;
	uint32_t quad = 0;
	uint32_t hidden = 0;
	for (uint32_t y = 0; y < HEIGHT; y++) {
		for (uint32_t x = 0; x < WIDTH; x++) {
			const uint32_t stored = target[tiled_index(x, y, WIDTH)];
			quad += same_colour(stored, __builtin_bswap32(QUAD_ARGB));
			hidden += same_colour(stored, __builtin_bswap32(HIDDEN_ARGB));
		}
	}
	xsdk_log_write("occlusion probe: %s: of %u pixels, %u the quad's colour, %u the hidden quad's", what,
		(unsigned)PIXELS, (unsigned)quad, (unsigned)hidden);
	return true;
}

// The quad and the one hidden behind it, without a query, to count what the queries are to count.
static bool reference(void)
{
	if (!begin())
		return false;
	draw_quad(0, 0, 0.5f, QUAD_ARGB);
	draw_quad(0, 0, 0.75f, HIDDEN_ARGB);
	xsdk_log_write(
		"occlusion probe: the quad covers %u pixels, and the hidden quad is behind it", (unsigned)QUAD_PIXELS);
	return count_pixels("the reference draws");
}

// The four counters as the CPU reads them, and as the command processor copies them once the GPU is idle.
static bool read_counters(const char *when, uint32_t copy)
{
	static const uint16_t counters[] = { RB_TOTAL_SAMPLES, RB_ZPASS_SAMPLES, RB_ZFAIL_SAMPLES, RB_SFAIL_SAMPLES };
	put_wait_idle();
	for (uint32_t i = 0; i < sizeof counters / sizeof counters[0]; i++)
		put_copy(counters[i], COPY_WORD + copy + i);
	if (!step(when, false))
		return false;
	volatile const uint32_t *const registers = probe_gpu_registers;
	volatile const uint32_t *const words = &memory[COPY_WORD + copy];
	xsdk_log_write(
		"occlusion probe: %s: total, z pass, z fail and stencil fail read %08X %08X %08X %08X, copied %08X "
		"%08X %08X %08X",
		when, (unsigned)registers[RB_TOTAL_SAMPLES], (unsigned)registers[RB_ZPASS_SAMPLES],
		(unsigned)registers[RB_ZFAIL_SAMPLES], (unsigned)registers[RB_SFAIL_SAMPLES], (unsigned)words[0],
		(unsigned)words[1], (unsigned)words[2], (unsigned)words[3]);
	return true;
}

// Yamato's counters after the clear, the quad and the hidden quad; then after RB_SAMPLE_COUNT_CTL's RESET, and a quad
// after it.
static bool probe_registers(void)
{
	if (!begin())
		return false;
	mark_unwritten();
	if (!read_counters("after the clear", 0))
		return false;
	draw_quad(0, 0, 0.5f, QUAD_ARGB);
	if (!read_counters("after the quad", 4))
		return false;
	draw_quad(0, 0, 0.75f, HIDDEN_ARGB);
	if (!read_counters("after the hidden quad", 8))
		return false;
	put_register(RB_SAMPLE_COUNT_CTL, SAMPLE_COUNT_RESET);
	if (!read_counters("after RESET", 12))
		return false;
	draw_quad(512, 256, 0.5f, QUAD_ARGB);
	return read_counters("after a quad after RESET", 16);
}

static void put_viz(uint32_t id_and_end)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_VIZ_QUERY, 1);
	packet[1] = id_and_end;
}

static void begin_viz(uint32_t id, uint32_t kill)
{
	put_register(PA_SC_VIZ_QUERY, VIZ_QUERY_ENA | id << VIZ_QUERY_ID_SHIFT | kill);
	put_viz(id);
}

// The query's end, its status copied at once and again once the GPU is idle, logged with the CPU's read after.
static bool end_viz(uint32_t id, const char *what)
{
	put_viz(id | VIZ_QUERY_END);
	put_register(PA_SC_VIZ_QUERY, 0);
	put_copy(PA_SC_VIZ_QUERY_STATUS_0, COPY_WORD + 2 * id);
	put_wait_idle();
	put_copy(PA_SC_VIZ_QUERY_STATUS_0, COPY_WORD + 2 * id + 1);
	if (!step(what, false))
		return false;
	volatile const uint32_t *const registers = probe_gpu_registers;
	const uint32_t status = registers[PA_SC_VIZ_QUERY_STATUS_0];
	xsdk_log_write("occlusion probe: %s: query %u's bit %u; status %08X %08X; copied at its end %08X, once idle %08X",
		what, (unsigned)id, (unsigned)(status >> id & 1), (unsigned)status,
		(unsigned)registers[PA_SC_VIZ_QUERY_STATUS_1], (unsigned)memory[COPY_WORD + 2 * id],
		(unsigned)memory[COPY_WORD + 2 * id + 1]);
	return true;
}

// Queries around the quad, the hidden quad, nothing, and a quad elsewhere killed once tested; then what drew.
static bool probe_viz_query(void)
{
	if (!begin())
		return false;
	mark_unwritten();
	volatile const uint32_t *const registers = probe_gpu_registers;
	xsdk_log_write("occlusion probe: viz status before the queries: %08X %08X",
		(unsigned)registers[PA_SC_VIZ_QUERY_STATUS_0], (unsigned)registers[PA_SC_VIZ_QUERY_STATUS_1]);
	begin_viz(1, 0);
	draw_quad(0, 0, 0.5f, QUAD_ARGB);
	if (!end_viz(1, "the quad"))
		return false;
	begin_viz(2, 0);
	draw_quad(0, 0, 0.75f, HIDDEN_ARGB);
	if (!end_viz(2, "the hidden quad"))
		return false;
	begin_viz(3, 0);
	if (!end_viz(3, "nothing drawn"))
		return false;
	begin_viz(4, KILL_PIX_POST_HI_Z);
	draw_quad(512, 256, 0.5f, QUAD_ARGB);
	if (!end_viz(4, "a quad killed once tested"))
		return false;
	return count_pixels("the queries' draws, the first quad's alone if the kill held");
}

// RB_SAMPLE_COUNT_ADDR at the slot, the control, then the event.
static void put_counts(uint32_t slot, uint32_t control)
{
	put_register(RB_SAMPLE_COUNT_ADDR, memory_address + slot * SLOT_WORDS * 4);
	put_register(RB_SAMPLE_COUNT_CTL, control);
	volatile uint32_t *const packet = xsdk_gpu_reserve(2);
	packet[0] = xsdk_gpu_type3(PM4_EVENT_WRITE_ZPD, 1);
	packet[1] = ZPASS_DONE;
	events_sent++;
}

// An event's fences, once the pixel shaders are done and once the GPU is idle, then its slot.
static bool settle(uint32_t slot, const char *what)
{
	const bool landed = step(what, false) && step(what, true);
	events_written += slot_written(slot);
	log_slot(slot, what);
	return landed;
}

static void put_values(uint32_t group_mask, const uint32_t values[GROUPS][GROUP_MAX_REGISTERS])
{
	put_wait_idle();
	for (uint32_t group = 0; group < GROUPS; group++) {
		if (!(group_mask & 1 << group))
			continue;
		for (uint32_t i = 0; i < groups[group].count; i++)
			put_register(groups[group].indices[i], values[group][i]);
	}
}

// xsdk's state after an event: below 0x2000 as it read back; above, what the set-up alone writes, then a resolve and
// the draw state, which write the rest and reload the shaders.
static void put_ours(uint32_t group_mask)
{
	put_values(group_mask & CONTROL_GROUPS, opened);
	if (!(group_mask & ~CONTROL_GROUPS))
		return;
	put_register(PA_SC_SCREEN_SCISSOR_TL, 0);
	put_register(PA_SC_SCREEN_SCISSOR_BR, SET_UP_SCREEN_SCISSOR_BR);
	put_register(PA_SU_POINT_SIZE, SET_UP_POINT_SIZE);
	put_register(PA_SU_VTX_CNTL, SET_UP_PIX_CENTER_D3D);
	put_register(SQ_VS_CONST, SET_UP_VS_CONST);
	put_register(SQ_PS_CONST, SET_UP_PS_CONST);
	copy_out();
}

static void put_counters(bool on)
{
	const uint32_t control = opened[GROUP_BACKEND][0];
	put_wait_idle();
	put_register(RB_BC_CONTROL, on ? control : control | DISABLE_SAMPLE_COUNTERS);
}

static bool first_event(const Try *current)
{
	if (current->first == FIRST_RESET) {
		put_counts(LONE_SLOT, SAMPLE_COUNT_RESET);
		return settle(LONE_SLOT, "a lone RESET");
	}
	if (current->first == FIRST_RST_PIX_CNT) {
		put_event(RST_PIX_CNT);
		return step("RST_PIX_CNT", false) && step("RST_PIX_CNT", true);
	}
	return true;
}

// One event of the counting, with the try's state around it.
static bool count_into(const Try *current, uint32_t slot, const char *after)
{
	char what[STEP_NAME_BYTES];
	(void)snprintf(what, sizeof what, "slot %u, after %s", (unsigned)slot, after);
	if (current->groups)
		put_values(current->groups, found);
	put_counts(slot, current->control);
	if (current->after == AFTER_FLUSH)
		put_event(CACHE_FLUSH);
	else if (current->after == AFTER_RESOLVE)
		copy_out();
	if (!settle(slot, what))
		return false;
	if (current->groups)
		put_ours(current->groups);
	return true;
}

// Counts after the clear, after the quad, and after the quad hidden behind it.
static bool probe_zpass(const Try *current)
{
	if (!begin())
		return false;
	mark_unwritten();
	if (current->counters_off)
		put_counters(false);
	if (!first_event(current) || !count_into(current, 0, "the clear"))
		return false;
	draw_quad(0, 0, 0.5f, QUAD_ARGB);
	if (!count_into(current, 1, "the quad"))
		return false;
	draw_quad(0, 0, 0.75f, HIDDEN_ARGB);
	if (!count_into(current, 2, "the hidden quad"))
		return false;
	if (current->counters_off)
		put_counters(true);
	if (!step("the counting's end", true))
		return false;
	for (uint32_t slot = 0; slot < SLOTS; slot++)
		log_slot(slot, "at the end");
	log_interval(0, "the quad");
	log_interval(1, "the hidden quad");
	return true;
}

static void read_groups(uint32_t values[GROUPS][GROUP_MAX_REGISTERS])
{
	for (uint32_t group = 0; group < GROUPS; group++) {
		for (uint32_t i = 0; i < groups[group].count; i++)
			values[group][i] = probe_gpu_registers[groups[group].indices[i]];
	}
}

// Each group's registers: the index, as found, then once set up. Below 0x2000, a group that doesn't differ can't change
// a try.
static void log_groups(void)
{
	for (uint32_t group = 0; group < GROUPS; group++) {
		char line[GROUP_LINE_BYTES] = "";
		size_t length = 0;
		uint32_t differ = 0;
		for (uint32_t i = 0; i < groups[group].count; i++) {
			length += (size_t)snprintf(&line[length], sizeof line - length, " %04X %08X %08X",
				(unsigned)groups[group].indices[i], (unsigned)found[group][i], (unsigned)opened[group][i]);
			differ += found[group][i] != opened[group][i];
		}
		xsdk_log_write("occlusion probe: %s: %u of %u differ, found then set up:%s", groups[group].name,
			(unsigned)differ, (unsigned)groups[group].count, line);
	}
}

static const Try *find_try(const char *name)
{
	for (uint32_t i = 0; i < sizeof tries / sizeof tries[0]; i++) {
		if (strcmp(tries[i].name, name) == 0)
			return &tries[i];
	}
	return NULL;
}

static bool run(const Try *current)
{
	if (current->kind == KIND_REGISTERS)
		return probe_registers();
	if (current->kind == KIND_VIZ_QUERY)
		return probe_viz_query();
	return probe_zpass(current);
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
	xsdk_log_write("occlusion probe: try %s", OCCLUSION_TRY);
	const Try *const current = find_try(OCCLUSION_TRY);
	if (!current) {
		xsdk_log_write("occlusion probe: no try is named %s", OCCLUSION_TRY);
		return 1;
	}
	// As the title finds them; xsdk's set-up reads its own once VdInitializeEngines has the GPU (src/gpu.c).
	read_groups(found);
	const NTSTATUS status = xsdk_gpu_open();
	xsdk_log_write("occlusion probe: GPU 0x%08X", (unsigned)status);
	if (!NT_SUCCESS(status))
		return 1;
	if (!set_up()) {
		xsdk_log_write("occlusion probe: set-up failed");
		return 1;
	}
	read_groups(opened);
	log_groups();
	const uint32_t control = probe_gpu_registers[RB_SAMPLE_COUNT_CTL];
	report("set up");
	const bool finished = reference() && run(current);
	if (finished)
		put_register(RB_SAMPLE_COUNT_CTL, control);
	xsdk_log_write("occlusion probe: %s: %s; %u of %u events wrote their counts", current->name, stopped_at,
		(unsigned)events_written, (unsigned)events_sent);
	xsdk_log_write("occlusion probe: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
