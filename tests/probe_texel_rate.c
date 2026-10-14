#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "texel_shaders.h"
#include "texture_shaders.h"
#include "tiled_index.h"
#include "xsdk/gpu.h"
#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/shader.h"
#include "xsdk/texture.h"

// Times four samples a pixel, at coordinates each pixel shader works out, laid out a way each
// (tools/gen_texel_shaders.py; docs/scope/36-texel-rate.md): fills of a 1280x720 target, checked after each, then with
// more registers declared, under a split giving pixel shaders more, and from a 64-bit texture filtered between two
// levels. Last, whether xost's build of the same GLSL runs within 5% of the fastest hand-built layout.

enum {
	WIDTH = 1280,
	HEIGHT = 720,
	PIXELS = WIDTH * HEIGHT,
	SAMPLES = 4,
	// The GPU's published clock, and its texture units' published 16 bilinear samples a clock.
	GPU_MHZ = 500,
	// How much slower than the fastest hand-built layout xost's build may run, in percent.
	XOST_SLACK_PERCENT = 5,
	BATCH_US = 100000,
	CALIBRATION_UNITS = 4,
	WARM_UP_UNITS = 2,
	MAX_UNITS = 4000,
	FENCE_TIMEOUT_US = 2000000,
	// Looks at the fence this far apart cost the GPU nothing (docs/HARDWARE.md, "Looking at a fence costs the GPU").
	FENCE_LOOK_US = 20,
	CLEAR_ARGB = 0xFF202020,
	CHANNEL_TOLERANCE = 3,
	VERTEX_FLOATS = 6,
	STRIDE_BYTES = VERTEX_FLOATS * 4,
	TEXCOORD_OFFSET_BYTES = 16,
	QUAD_VERTICES = 6,
	QUAD_BYTES = QUAD_VERTICES * STRIDE_BYTES,
	TEXTURE_SIZE = 256,
	TEXTURE_SLOT = 0,
	// The 64-bit texture: 512 texels a side with every level, drawn about 1.41 texels a pixel, so trilinear filtering
	// blends levels 0 and 1.
	WIDE_SIZE = 512,
	WIDE_LEVELS = 10,
	// Half floats: 0.5 and 1.0.
	HALF_HALF = 0x3800,
	HALF_ONE = 0x3C00,
};

// Registers by index (xenia:src/xenia/gpu/register_table.inc).
enum {
	WAIT_UNTIL = 0x05C8,
	SQ_GPR_MANAGEMENT = 0x0D00,
};

enum {
	WAIT_3D_IDLE_CLEAN = 1 << 17,
	// A type-0 header's bit 15 sends every value to the same register (libxenon:drivers/xenos/xe.c:25-31).
	TYPE0_ONE_REGISTER = 1 << 15,
	// Vertex shaders' share in bits 12-19, pixel shaders' in bits 4-11, out of 128 (src/gpu_setup.c).
	SPLIT_TOTAL = 0x80,
	SPLIT_XSDK_VERTEX = 0x40,
	SPLIT_PIXEL_HEAVY_VERTEX = 0x10,
	SPLIT_REPEATS = 8,
	SPLIT_DYNAMIC = 1,
	// SQ_PROGRAM_CNTL's pixel shader register count less one (xenia:src/xenia/gpu/registers.h:136).
	PS_REGISTERS_SHIFT = 8,
	PS_REGISTERS_MASK = 0x3F << PS_REGISTERS_SHIFT,
};

typedef struct {
	const char *name;
	const uint32_t *container;
	uint32_t bytes;
	// Every sample at the interpolated coordinates, so each pixel is its own texel, not the average of four.
	bool same_coordinates;
} Layout;

static const Layout layouts[] = {
	{ "reference, four fetches at the interpolated coordinates", texel_reference_shader, sizeof texel_reference_shader,
		true },
	{ "xost's before milestone 36, each add then its fetch, all through r2", texel_xost_shader,
		sizeof texel_xost_shader, false },
	{ "xost's before milestone 36, waiting", texel_xost_wait_shader, sizeof texel_xost_wait_shader, false },
	{ "each coordinate in its own register", texel_own_shader, sizeof texel_own_shader, false },
	{ "each coordinate in its own register, waiting", texel_own_wait_shader, sizeof texel_own_wait_shader, false },
	{ "the coordinates first, the fetches together", texel_together_shader, sizeof texel_together_shader, false },
	{ "the coordinates first, the fetches together, waiting", texel_together_wait_shader,
		sizeof texel_together_wait_shader, false },
	{ "two coordinates to a register, the fetches together", texel_paired_shader, sizeof texel_paired_shader, false },
	{ "two coordinates to a register, the fetches together, waiting", texel_paired_wait_shader,
		sizeof texel_paired_wait_shader, false },
	{ "xost's build of the GLSL", texel_xost_build_shader, sizeof texel_xost_build_shader, false },
};

enum {
	LAYOUTS = sizeof layouts / sizeof layouts[0],
	// The hand-built layouts of the four coordinates, those xost's build is held to, after the reference.
	FIRST_HAND_BUILT = 1,
	XOST_BUILD = LAYOUTS - 1,
	// The layouts without the wait bit, which the registers and the 64-bit texture are tried with.
	PLAIN_LAYOUTS = 6,
};

static const uint32_t plain_layouts[PLAIN_LAYOUTS] = { 0, 1, 3, 5, 7, XOST_BUILD };

enum {
	FILTER_POINT,
	FILTER_BILINEAR,
	FILTER_TRILINEAR_64,
	FILTERS,
};

static const char *const filter_names[FILTERS] = { "point", "bilinear", "trilinear, 64-bit" };

static const XsdkVertexElement vertex_layout[] = {
	{ .usage = XSDK_USAGE_POSITION, .usage_index = 0, .format = XSDK_VERTEX_FLOAT4, .offset_bytes = 0 },
	{ .usage = XSDK_USAGE_TEXCOORD,
		.usage_index = 0,
		.format = XSDK_VERTEX_FLOAT2,
		.offset_bytes = TEXCOORD_OFFSET_BYTES },
};

// c0 a texel's step across and down, c1 the quarter that averages the four.
static const float narrow_constants[8] = { 1.0f / TEXTURE_SIZE, 1.0f / TEXTURE_SIZE, 0.0f, 0.0f, 0.25f, 0.25f, 0.25f,
	0.25f };
static const float wide_constants[8] = { 1.0f / WIDE_SIZE, 1.0f / WIDE_SIZE, 0.0f, 0.0f, 0.25f, 0.25f, 0.25f, 0.25f };

static XsdkShader vertex_shader;
static XsdkShader shaders[LAYOUTS];
static XsdkTexture narrow;
static XsdkTexture wide;
static XsdkSamplerState samplers[FILTERS];
static XsdkDepthStencilState depth_off;
static XsdkBlendState opaque;
static volatile uint32_t *vertices;
static volatile uint32_t *pixels;
static uint32_t narrow_quad_address;
static uint32_t wide_quad_address;
// Each layout's bilinear Mtexels/s with its own registers under xsdk's split.
static uint64_t bilinear_mtexels[LAYOUTS];

static uint64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000 + (uint64_t)now.tv_nsec / 1000;
}

// Spins on the time base between looks, never sleeping; false if the GPU never gets there.
static bool finish(void)
{
	const uint32_t fence = xsdk_gpu_fence();
	const uint64_t start = now_us();
	uint64_t now = start;
	while (!xsdk_gpu_fence_done(fence)) {
		if (now - start > FENCE_TIMEOUT_US)
			return false;
		const uint64_t next = now + FENCE_LOOK_US;
		while ((now = now_us()) < next) {
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

// One step of Xe_pGInit1: the split written 8 times with bit 0, then once without (libxenon xe.c:443-452).
static void put_split(uint32_t vertex_share)
{
	const uint32_t split = vertex_share << 12 | (SPLIT_TOTAL - vertex_share) << 4;
	put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
	volatile uint32_t *const packet = xsdk_gpu_reserve(1 + SPLIT_REPEATS);
	packet[0] = (SPLIT_REPEATS - 1) << 16 | TYPE0_ONE_REGISTER | SQ_GPR_MANAGEMENT;
	for (uint32_t i = 0; i < SPLIT_REPEATS; i++)
		packet[1 + i] = split | SPLIT_DYNAMIC;
	put_register(SQ_GPR_MANAGEMENT, split);
}

static void step_split(uint32_t from, uint32_t to)
{
	while (from != to) {
		from = from < to ? from + 1 : from - 1;
		put_split(from);
	}
}

static float clip_x(float x)
{
	return x / (WIDTH / 2.0f) - 1.0f;
}

static float clip_y(float y)
{
	return 1.0f - y / (HEIGHT / 2.0f);
}

static void put_vertex(volatile uint32_t *at, float x, float y, float u, float v)
{
	const float vertex[VERTEX_FLOATS] = { clip_x(x), clip_y(y), 0.0f, 1.0f, u, v };
	uint32_t words[VERTEX_FLOATS];
	memcpy(words, vertex, sizeof vertex);
	for (uint32_t i = 0; i < VERTEX_FLOATS; i++)
		at[i] = words[i];
}

// The whole target, edges half a pixel before the pixels for Direct3D centres (xenia:src/xenia/gpu/draw_util.cc:837),
// texels_per_pixel of a size-texel texture each pixel.
static void put_quad(volatile uint32_t *at, float texels_per_pixel, float size)
{
	const float u1 = WIDTH * texels_per_pixel / size;
	const float v1 = HEIGHT * texels_per_pixel / size;
	put_vertex(at, -0.5f, -0.5f, 0.0f, 0.0f);
	put_vertex(at + VERTEX_FLOATS, WIDTH - 0.5f, -0.5f, u1, 0.0f);
	put_vertex(at + 2 * VERTEX_FLOATS, -0.5f, HEIGHT - 0.5f, 0.0f, v1);
	put_vertex(at + 3 * VERTEX_FLOATS, -0.5f, HEIGHT - 0.5f, 0.0f, v1);
	put_vertex(at + 4 * VERTEX_FLOATS, WIDTH - 0.5f, -0.5f, u1, 0.0f);
	put_vertex(at + 5 * VERTEX_FLOATS, WIDTH - 0.5f, HEIGHT - 0.5f, u1, v1);
}

// Rises by 2 a texel to 255, then falls, so neighbours stay within 2 across the wrap too (as probe_pixel_rate.c).
static uint32_t ramp(uint32_t i)
{
	i %= TEXTURE_SIZE;
	return i < TEXTURE_SIZE / 2 ? 2 * i : 2 * (TEXTURE_SIZE - 1 - i) + 1;
}

static uint32_t texel(uint32_t x, uint32_t y)
{
	return 0xFF000080 | ramp(x) << 16 | ramp(y) << 8;
}

static bool make_textures(void)
{
	if (!NT_SUCCESS(xsdk_texture_create(&narrow, XSDK_FORMAT_A8R8G8B8, TEXTURE_SIZE, TEXTURE_SIZE, 1, true)))
		return false;
	static uint32_t texels[TEXTURE_SIZE * TEXTURE_SIZE];
	for (uint32_t y = 0; y < TEXTURE_SIZE; y++)
		for (uint32_t x = 0; x < TEXTURE_SIZE; x++)
			texels[y * TEXTURE_SIZE + x] = texel(x, y);
	xsdk_texture_upload(&narrow, 0, texels, TEXTURE_SIZE * 4);
	xsdk_gpu_invalidate(narrow.address, narrow.size_bytes);

	if (!NT_SUCCESS(xsdk_texture_create(&wide, XSDK_FORMAT_R16G16B16A16F, WIDE_SIZE, WIDE_SIZE, WIDE_LEVELS, true)))
		return false;
	// Each level a colour of its own, so the filter has two levels to blend.
	static uint16_t halves[WIDE_SIZE * WIDE_SIZE * 4];
	for (uint32_t level = 0; level < WIDE_LEVELS; level++) {
		const uint32_t size = WIDE_SIZE >> level;
		const uint16_t red = level % 2 ? HALF_ONE : HALF_HALF;
		for (uint32_t i = 0; i < size * size; i++) {
			halves[4 * i] = red;
			halves[4 * i + 1] = HALF_HALF;
			halves[4 * i + 2] = HALF_HALF;
			halves[4 * i + 3] = HALF_ONE;
		}
		xsdk_texture_upload(&wide, level, halves, size * 8);
	}
	xsdk_gpu_invalidate(wide.address, wide.size_bytes);
	return true;
}

static bool make_resources(void)
{
	if (!NT_SUCCESS(xsdk_shader_load(&vertex_shader, texture_vertex_shader, sizeof texture_vertex_shader)) ||
		!NT_SUCCESS(
			xsdk_shader_set_vertex_layout(&vertex_shader, vertex_layout, 2, (const uint32_t[]){ STRIDE_BYTES })))
		return false;
	for (uint32_t i = 0; i < LAYOUTS; i++) {
		if (!NT_SUCCESS(xsdk_shader_load(&shaders[i], layouts[i].container, layouts[i].bytes))) {
			xsdk_log_write("texel rate: %s won't load", layouts[i].name);
			return false;
		}
	}
	if (!make_textures())
		return false;
	vertices = xsdk_gpu_alloc(2 * QUAD_BYTES);
	pixels = xsdk_gpu_alloc(xsdk_gpu_resolve_bytes(WIDTH, HEIGHT));
	if (!vertices || !pixels)
		return false;
	put_quad(vertices, 1.0f, TEXTURE_SIZE);
	put_quad(vertices + QUAD_BYTES / 4, 1.41421356f, WIDE_SIZE);
	__asm__ volatile("sync" ::: "memory");
	narrow_quad_address = xsdk_gpu_address(vertices);
	wide_quad_address = narrow_quad_address + QUAD_BYTES;
	xsdk_gpu_invalidate(narrow_quad_address, 2 * QUAD_BYTES);

	const XsdkSampler point = { XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_FILTER_POINT, XSDK_ADDRESS_WRAP,
		XSDK_ADDRESS_WRAP, 0.0f, 0 };
	const XsdkSampler bilinear = { XSDK_FILTER_LINEAR, XSDK_FILTER_LINEAR, XSDK_FILTER_POINT, XSDK_ADDRESS_WRAP,
		XSDK_ADDRESS_WRAP, 0.0f, 0 };
	const XsdkSampler trilinear = { XSDK_FILTER_LINEAR, XSDK_FILTER_LINEAR, XSDK_FILTER_LINEAR, XSDK_ADDRESS_WRAP,
		XSDK_ADDRESS_WRAP, 0.0f, 0 };
	xsdk_texture_sampler_state(&samplers[FILTER_POINT], &point);
	xsdk_texture_sampler_state(&samplers[FILTER_BILINEAR], &bilinear);
	xsdk_texture_sampler_state(&samplers[FILTER_TRILINEAR_64], &trilinear);
	const XsdkDepthStencil off = { .depth_compare = XSDK_COMPARE_ALWAYS, .stencil_compare = XSDK_COMPARE_ALWAYS };
	xsdk_gpu_depth_stencil_state(&depth_off, &off);
	const XsdkBlend blend = { XSDK_BLEND_ONE, XSDK_BLEND_ZERO, XSDK_BLEND_OP_ADD, XSDK_BLEND_ONE, XSDK_BLEND_ZERO,
		XSDK_BLEND_OP_ADD, XSDK_WRITE_ALL };
	xsdk_gpu_blend_state(&opaque, &blend);
	return true;
}

static void set_state(const XsdkShader *shader, uint32_t filter)
{
	const bool wide_texture = filter == FILTER_TRILINEAR_64;
	xsdk_gpu_set_target(WIDTH, HEIGHT);
	xsdk_gpu_set_shaders(&vertex_shader, shader);
	xsdk_gpu_set_pixel_constants(0, wide_texture ? wide_constants : narrow_constants, 2);
	xsdk_gpu_set_vertices(0, wide_texture ? wide_quad_address : narrow_quad_address, QUAD_BYTES);
	xsdk_gpu_set_texture(TEXTURE_SLOT, wide_texture ? &wide : &narrow, &samplers[filter]);
	xsdk_gpu_set_depth_stencil(&depth_off);
	xsdk_gpu_set_blend(&opaque);
}

// Microseconds the GPU took for count full-screen draws; 0 if it hung.
static uint64_t time_draws(uint32_t count)
{
	const uint64_t start = now_us();
	for (uint32_t i = 0; i < count; i++)
		xsdk_gpu_draw(XSDK_PRIMITIVE_TRIANGLE_LIST, QUAD_VERTICES);
	if (!finish())
		return 0;
	const uint64_t elapsed = now_us() - start;
	return elapsed ? elapsed : 1;
}

static uint32_t stored_argb(uint32_t x, uint32_t y)
{
	return __builtin_bswap32(pixels[tiled_index(x, y, WIDTH)]);
}

// The average of four colours, channel by channel, rounded.
static uint32_t average(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
	uint32_t argb = 0;
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t sum = (a >> shift & 0xFF) + (b >> shift & 0xFF) + (c >> shift & 0xFF) + (d >> shift & 0xFF);
		argb |= (sum + 2) / 4 << shift;
	}
	return argb;
}

static uint32_t expected_argb(const Layout *layout, uint32_t x, uint32_t y)
{
	return layout->same_coordinates ? texel(x, y)
									: average(texel(x, y), texel(x + 1, y), texel(x, y + 1), texel(x + 1, y + 1));
}

static bool near(uint32_t a, uint32_t b)
{
	for (uint32_t shift = 0; shift < 32; shift += 8) {
		const uint32_t channel_a = a >> shift & 0xFF;
		const uint32_t channel_b = b >> shift & 0xFF;
		if ((channel_a > channel_b ? channel_a - channel_b : channel_b - channel_a) > CHANNEL_TOLERANCE)
			return false;
	}
	return true;
}

static const uint32_t check_points[][2] = { { 0, 0 }, { 640, 360 }, { 1279, 719 }, { 300, 500 }, { 637, 357 } };

enum {
	CHECK_POINTS = sizeof check_points / sizeof check_points[0],
};

// Resolves what the draws left and checks it; the 64-bit texture's blend isn't checked, only that something drew.
static bool check(const Layout *layout, uint32_t filter, char *outcome, size_t size)
{
	xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
	if (!finish()) {
		(void)snprintf(outcome, size, "the resolve didn't finish");
		return false;
	}
	for (uint32_t i = 0; i < CHECK_POINTS; i++) {
		const uint32_t x = check_points[i][0];
		const uint32_t y = check_points[i][1];
		const uint32_t stored = stored_argb(x, y);
		const bool right =
			filter == FILTER_TRILINEAR_64 ? stored != CLEAR_ARGB : near(stored, expected_argb(layout, x, y));
		if (!right) {
			(void)snprintf(outcome, size, "WRONG: %08X at (%u, %u)", (unsigned)stored, (unsigned)x, (unsigned)y);
			return false;
		}
	}
	(void)snprintf(outcome, size, filter == FILTER_TRILINEAR_64 ? "drew" : "drew right");
	return true;
}

// One measurement: a warm-up, a short batch to size the timed one, then about 100 ms of draws, then the check.
static bool measure(uint32_t index, uint32_t registers, uint32_t vertex_share, uint32_t filter)
{
	const Layout *layout = &layouts[index];
	XsdkShader shader = shaders[index];
	const uint32_t own_registers = ((shader.program_control & PS_REGISTERS_MASK) >> PS_REGISTERS_SHIFT) + 1;
	if (registers)
		shader.program_control =
			(shader.program_control & ~(uint32_t)PS_REGISTERS_MASK) | (registers - 1) << PS_REGISTERS_SHIFT;
	if (vertex_share != SPLIT_XSDK_VERTEX) {
		put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
		step_split(SPLIT_XSDK_VERTEX, vertex_share);
	}
	set_state(&shader, filter);
	const uint64_t warm_us = time_draws(WARM_UP_UNITS);
	const uint64_t calibration_us = warm_us ? time_draws(CALIBRATION_UNITS) : 0;
	uint64_t count = calibration_us ? BATCH_US * CALIBRATION_UNITS / calibration_us : 0;
	count = count < CALIBRATION_UNITS ? CALIBRATION_UNITS : count > MAX_UNITS ? MAX_UNITS : count;
	const uint64_t elapsed_us = calibration_us ? time_draws((uint32_t)count) : 0;
	char outcome[64] = "the GPU didn't finish";
	const bool drew = elapsed_us && check(layout, filter, outcome, sizeof outcome);
	if (vertex_share != SPLIT_XSDK_VERTEX) {
		put_register(WAIT_UNTIL, WAIT_3D_IDLE_CLEAN);
		step_split(vertex_share, SPLIT_XSDK_VERTEX);
	}
	const uint64_t mpixels = elapsed_us ? (uint64_t)PIXELS * count / elapsed_us : 0;
	const uint64_t mtexels = mpixels * SAMPLES;
	xsdk_log_write(
		"texel rate: %s, %u registers%s, split %u pixel: %s: %u draws in %u us: %u Mtexels/s, %u.%02u a "
		"clock; %s",
		layout->name, (unsigned)(registers ? registers : own_registers), registers ? " declared" : "",
		(unsigned)(SPLIT_TOTAL - vertex_share), filter_names[filter], (unsigned)count, (unsigned)elapsed_us,
		(unsigned)mtexels, (unsigned)(mtexels / GPU_MHZ), (unsigned)(mtexels * 100 / GPU_MHZ % 100), outcome);
	if (!registers && vertex_share == SPLIT_XSDK_VERTEX && filter == FILTER_BILINEAR)
		bilinear_mtexels[index] = drew ? mtexels : 0;
	return elapsed_us && finish() && (drew || filter == FILTER_TRILINEAR_64);
}

// Logs whether xost's build, bilinear, ran within XOST_SLACK_PERCENT of the fastest hand-built layout.
static void judge_xost(void)
{
	uint32_t fastest = FIRST_HAND_BUILT;
	for (uint32_t i = FIRST_HAND_BUILT; i < XOST_BUILD; i++)
		if (bilinear_mtexels[i] > bilinear_mtexels[fastest])
			fastest = i;
	const uint64_t built = bilinear_mtexels[XOST_BUILD];
	const uint64_t best = bilinear_mtexels[fastest];
	const bool close = best && built * 100 >= best * (100 - XOST_SLACK_PERCENT);
	xsdk_log_write("texel rate: xost's build %u.%02u a clock, the fastest hand-built (%s) %u.%02u: %s",
		(unsigned)(built / GPU_MHZ), (unsigned)(built * 100 / GPU_MHZ % 100), layouts[fastest].name,
		(unsigned)(best / GPU_MHZ), (unsigned)(best * 100 / GPU_MHZ % 100),
		close ? "within 5%" : "FAIL, more than 5% slower");
}

int main(void)
{
	xsdk_log_write("texel rate probe");
	const NTSTATUS status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("texel rate: GPU 0x%08X", (unsigned)status);
		return 1;
	}
	if (!make_resources()) {
		xsdk_log_write("texel rate: setup failed");
		return 1;
	}
	xsdk_gpu_resolve(xsdk_gpu_address(pixels), WIDTH, HEIGHT, CLEAR_ARGB);
	bool ran = finish();
	// Every layout, point and bilinear, with its own registers under xsdk's split.
	for (uint32_t i = 0; i < LAYOUTS && ran; i++)
		for (uint32_t filter = FILTER_POINT; filter <= FILTER_BILINEAR && ran; filter++)
			ran = measure(i, 0, SPLIT_XSDK_VERTEX, filter);
	// Those without the wait bit, bilinear, with 12 and 17 registers declared, under xsdk's split and a pixel-heavy
	// one.
	static const uint32_t registers[] = { 0, 12, 17 };
	for (uint32_t i = 0; i < PLAIN_LAYOUTS && ran; i++)
		for (uint32_t r = 0; r < 3 && ran; r++)
			for (uint32_t split = 0; split < 2 && ran; split++) {
				if (r == 0 && split == 0)
					continue;
				ran = measure(plain_layouts[i], registers[r], split ? SPLIT_PIXEL_HEAVY_VERTEX : SPLIT_XSDK_VERTEX,
					FILTER_BILINEAR);
			}
	// The 64-bit texture, trilinear, with their own registers and 12.
	for (uint32_t i = 0; i < PLAIN_LAYOUTS && ran; i++)
		for (uint32_t r = 0; r < 2 && ran; r++)
			ran = measure(plain_layouts[i], registers[r], SPLIT_XSDK_VERTEX, FILTER_TRILINEAR_64);
	if (ran)
		judge_xost();
	xsdk_log_write("texel rate: %s", ran ? "done" : "stopped");
	xsdk_log_write("texel rate: GPU close 0x%08X", (unsigned)xsdk_gpu_close());
	return 0;
}
