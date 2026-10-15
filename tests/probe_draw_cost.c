#include <altivec.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "xsdk/gpu.h"
#include "xsdk/log.h"

// Times what vkbench's cube costs the CPU through the GPU layer, a call at a time (SCOPE.md, milestone 38): recorded
// into a command buffer the GPU never runs, so only the CPU's work counts; then the cube's constants through the
// stream, with the GPU open, so its segments and kicks count too.

enum {
	ROUNDS = 9,
	CUBES = 4000,
	// The time base ticks every 64 CPU clocks: 3192 MHz over 49.875 MHz (src/time.c).
	CLOCKS_PER_TICK = 64,
	CLOCKS_PER_US = 3192,
	// What a cube writes through the layer (vkbench's ring counts, docs/scope/38-draw-cost.md).
	CUBE_DWORDS = 49,
	// Room for the most any variant records.
	BUFFER_DWORDS = CUBES * 64,
	CUBE_INDICES = 36,
	INDEX_BYTES = CUBE_INDICES * 2,
	MVP_VECTORS = 4,
	MVP_FLOATS = MVP_VECTORS * 4,
	TINT_FLOATS = 4,
	// vkbench's pixel shaders take the tint 64 bytes into their push constants, register 4.
	TINT_REGISTER = 4,
	FENCE_TIMEOUT_MS = 1000,
	// A group of 4 float constants, as the console takes them (docs/HARDWARE.md, "Float constants go in groups of 4").
	GROUP_DWORDS = 16,
	TINT_DWORDS = 4,
	// The layer's padding around a group: up to 3 NOPs before the header so the values start on 16 bytes, the rest
	// after (src/gpu_constants.c).
	ALIGN_DWORDS = 3,
	HEADER_SLOT = 3,
	TYPE2_NOP = 0x80000000,
	// SHADER_CONSTANT_000_X and SHADER_CONSTANT_256_X (src/gpu_private.h).
	VERTEX_CONSTANT_REGISTER = 0x4000,
	PIXEL_CONSTANT_REGISTER = 0x4400,
};

typedef __vector unsigned int Quad;

// Each cube's own matrix and tint, as vkbench's draw items hold them.
typedef struct {
	float mvp[MVP_FLOATS];
	float tint[TINT_FLOATS];
} Cube;

typedef struct {
	const char *name;
	void (*run)(void);
	// Recorded, or through the stream with the GPU open.
	bool live;
} Variant;

static Cube cubes[CUBES] __attribute__((aligned(16)));
// What a candidate writer keeps of each stage's first two groups, as the layer's shadow.
static uint32_t kept[2][2 * GROUP_DWORDS] __attribute__((aligned(16)));
static uint32_t index_address;
static XsdkCommandBuffer buffer;

static void set_vertex_constants(const Cube *cube)
{
	xsdk_gpu_set_vertex_constants(0, cube->mvp, MVP_VECTORS);
}

static void set_pixel_constants(const Cube *cube)
{
	xsdk_gpu_set_pixel_constants(TINT_REGISTER, cube->tint, 1);
}

static void draw(void)
{
	xsdk_gpu_draw_indexed(XSDK_PRIMITIVE_TRIANGLE_LIST, index_address, CUBE_INDICES, false);
}

static void run_vertex_constants(void)
{
	for (uint32_t c = 0; c < CUBES; c++)
		set_vertex_constants(&cubes[c]);
}

static void run_pixel_constants(void)
{
	for (uint32_t c = 0; c < CUBES; c++)
		set_pixel_constants(&cubes[c]);
}

static void run_draws(void)
{
	for (uint32_t c = 0; c < CUBES; c++)
		draw();
}

static void run_constants(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		set_vertex_constants(&cubes[c]);
		set_pixel_constants(&cubes[c]);
	}
}

static void run_cubes(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		set_vertex_constants(&cubes[c]);
		set_pixel_constants(&cubes[c]);
		draw();
	}
}

// A cube's dwords stored straight into the space reserved for them: one NOP packet, so a GPU that runs it skips the
// rest.
static void run_straight(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		volatile uint32_t *const space = xsdk_gpu_reserve(CUBE_DWORDS);
		space[0] = xsdk_gpu_type3(PM4_NOP, CUBE_DWORDS - 1);
		for (uint32_t i = 1; i < CUBE_DWORDS; i++)
			space[i] = c + i;
	}
}

// The same dwords in three reservations, as a cube's three calls make them: 20, 20, then 9, each a NOP packet, as the
// GPU runs them through the stream.
static void run_straight_three(void)
{
	static const uint32_t parts[] = { 20, 20, CUBE_DWORDS - 40 };
	for (uint32_t c = 0; c < CUBES; c++) {
		for (uint32_t p = 0; p < sizeof parts / sizeof parts[0]; p++) {
			volatile uint32_t *const space = xsdk_gpu_reserve(parts[p]);
			space[0] = xsdk_gpu_type3(PM4_NOP, parts[p] - 1);
			for (uint32_t i = 1; i < parts[p]; i++)
				space[i] = c + i;
		}
	}
}

// As xsdk_gpu_write takes the packets the layer builds on the stack: out of line, so they're stored before it reads
// them.
__attribute__((noinline)) static void copy_in(const uint32_t *dwords, uint32_t count)
{
	volatile uint32_t *const space = xsdk_gpu_reserve(count);
	for (uint32_t i = 0; i < count; i++)
		space[i] = dwords[i];
}

// The same dwords built in an array on the stack, then copied in.
static void run_staged(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		uint32_t dwords[CUBE_DWORDS];
		dwords[0] = xsdk_gpu_type3(PM4_NOP, CUBE_DWORDS - 1);
		for (uint32_t i = 1; i < CUBE_DWORDS; i++)
			dwords[i] = c + i;
		copy_in(dwords, CUBE_DWORDS);
	}
}

// The NOPs before a constant group's header, so its values start on 16 bytes; and the header.
static volatile uint32_t *begin_group(volatile uint32_t *packet, uint32_t first_register)
{
	const uint32_t before = (HEADER_SLOT - ((uint32_t)(uintptr_t)packet >> 2)) & 3;
	volatile uint32_t *out = packet;
	for (uint32_t i = 0; i < before; i++)
		*out++ = TYPE2_NOP;
	*out++ = (GROUP_DWORDS - 1) << 16 | first_register;
	return out;
}

static void end_group(volatile uint32_t *packet, volatile uint32_t *out)
{
	while (out < packet + ALIGN_DWORDS + 1 + GROUP_DWORDS)
		*out++ = TYPE2_NOP;
}

// A candidate for the layer's constants: one group, the first dwords new and the rest kept, 32-bit stores. With keep,
// the new dwords are kept too.
static void put_group_words(
	uint32_t first_register, const float *values, uint32_t new_dwords, uint32_t *group_kept, bool keep)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(ALIGN_DWORDS + 1 + GROUP_DWORDS);
	volatile uint32_t *out = begin_group(packet, first_register);
	for (uint32_t i = 0; i < new_dwords; i++) {
		uint32_t value;
		memcpy(&value, &values[i], sizeof value);
		if (keep)
			group_kept[i] = value;
		*out++ = value;
	}
	for (uint32_t i = new_dwords; i < GROUP_DWORDS; i++)
		*out++ = group_kept[i];
	end_group(packet, out);
}

// A vector from anywhere: the two quadwords it spans, put together.
static Quad load_quad(const float *values)
{
	const __vector unsigned char permute = vec_lvsl(0, values);
	return vec_perm(vec_ld(0, (const uint32_t *)values), vec_ld(15, (const uint32_t *)values), permute);
}

// The same with 16-byte stores, which the values' alignment allows.
static void put_group_quads(uint32_t first_register, const float *values, uint32_t new_dwords, uint32_t *group_kept)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(ALIGN_DWORDS + 1 + GROUP_DWORDS);
	volatile uint32_t *const out = begin_group(packet, first_register);
	for (uint32_t i = 0; i < GROUP_DWORDS; i += 4) {
		const Quad quad = i < new_dwords ? load_quad(&values[i]) : vec_ld(0, &group_kept[i]);
		if (i < new_dwords)
			vec_st(quad, 0, &group_kept[i]);
		vec_st(quad, 0, (uint32_t *)&out[i]);
	}
	end_group(packet, out + GROUP_DWORDS);
}

static void run_words(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		put_group_words(VERTEX_CONSTANT_REGISTER, cubes[c].mvp, GROUP_DWORDS, kept[0], false);
		put_group_words(
			PIXEL_CONSTANT_REGISTER + GROUP_DWORDS, cubes[c].tint, TINT_DWORDS, &kept[1][GROUP_DWORDS], false);
	}
}

static void run_words_kept(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		put_group_words(VERTEX_CONSTANT_REGISTER, cubes[c].mvp, GROUP_DWORDS, kept[0], true);
		put_group_words(
			PIXEL_CONSTANT_REGISTER + GROUP_DWORDS, cubes[c].tint, TINT_DWORDS, &kept[1][GROUP_DWORDS], true);
	}
}

static void run_quads(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		put_group_quads(VERTEX_CONSTANT_REGISTER, cubes[c].mvp, GROUP_DWORDS, kept[0]);
		put_group_quads(PIXEL_CONSTANT_REGISTER + GROUP_DWORDS, cubes[c].tint, TINT_DWORDS, &kept[1][GROUP_DWORDS]);
	}
}

static const Variant variants[] = {
	{ "vertex constants, 4 vectors", run_vertex_constants, false },
	{ "pixel constants, 1 vector", run_pixel_constants, false },
	{ "indexed draw", run_draws, false },
	{ "both constants", run_constants, false },
	{ "a cube: both constants and the draw", run_cubes, false },
	{ "49 dwords stored straight", run_straight, false },
	{ "49 dwords built on the stack, then copied", run_staged, false },
	{ "both constants, through the stream", run_constants, true },
	{ "49 dwords stored straight, three reservations", run_straight_three, false },
	{ "49 dwords stored straight, three reservations, through the stream", run_straight_three, true },
	{ "both constants, 32-bit stores, nothing kept", run_words, false },
	{ "both constants, 32-bit stores, the new values kept", run_words_kept, false },
	{ "both constants, 16-byte stores, the new values kept", run_quads, false },
};

enum { VARIANTS = sizeof variants / sizeof variants[0] };

static uint64_t ticks[VARIANTS][ROUNDS];
static uint32_t kicks[VARIANTS];

static uint64_t time_base(void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t check;
	// The upper half can tick between the two reads in 32-bit mode (src/time.c).
	do {
		__asm__ volatile("mftbu %0" : "=r"(upper));
		__asm__ volatile("mftb %0" : "=r"(lower));
		__asm__ volatile("mftbu %0" : "=r"(check));
	} while (upper != check);
	return (uint64_t)upper << 32 | lower;
}

static void fill_cubes(void)
{
	for (uint32_t c = 0; c < CUBES; c++) {
		for (uint32_t i = 0; i < MVP_FLOATS; i++)
			cubes[c].mvp[i] = (float)((c + i) % 17) * 0.125f;
		for (uint32_t i = 0; i < TINT_FLOATS; i++)
			cubes[c].tint[i] = (float)((c + i) % 5) * 0.25f;
	}
}

// One round of a variant; false if its recording overflowed or the GPU didn't finish its stream.
static bool time_variant(uint32_t variant, uint32_t round)
{
	const Variant *const v = &variants[variant];
	if (!v->live) {
		xsdk_gpu_record_begin(&buffer);
		const uint64_t start = time_base();
		v->run();
		ticks[variant][round] = time_base() - start;
		return NT_SUCCESS(xsdk_gpu_record_end());
	}
	XsdkGpuStats before;
	XsdkGpuStats after;
	xsdk_gpu_stats(&before);
	const uint64_t start = time_base();
	v->run();
	ticks[variant][round] = time_base() - start;
	xsdk_gpu_stats(&after);
	kicks[variant] = after.kicks - before.kicks;
	return NT_SUCCESS(xsdk_gpu_wait(xsdk_gpu_fence(), FENCE_TIMEOUT_MS));
}

static void sort_ticks(uint64_t *values, uint32_t count)
{
	for (uint32_t i = 1; i < count; i++) {
		const uint64_t value = values[i];
		uint32_t j = i;
		for (; j > 0 && values[j - 1] > value; j--)
			values[j] = values[j - 1];
		values[j] = value;
	}
}

static void report(uint32_t variant)
{
	uint64_t *const t = ticks[variant];
	sort_ticks(t, ROUNDS);
	const double best = (double)t[0] * CLOCKS_PER_TICK / CUBES;
	const double median = (double)t[ROUNDS / 2] * CLOCKS_PER_TICK / CUBES;
	xsdk_log_write("draw cost probe: %s: %.0f clocks a cube at best (%.3f us), %.0f the median", variants[variant].name,
		best, best / CLOCKS_PER_US, median);
	if (variants[variant].live)
		xsdk_log_write("draw cost probe: %s: %u kicks a round", variants[variant].name, (unsigned)kicks[variant]);
}

int main(void)
{
	xsdk_log_write("draw cost probe: %u cubes a round, %u rounds of %u variants", (unsigned)CUBES, (unsigned)ROUNDS,
		(unsigned)VARIANTS);
	fill_cubes();
	volatile uint32_t *const indices = xsdk_gpu_alloc(INDEX_BYTES);
	NTSTATUS status = xsdk_gpu_command_buffer_create(&buffer, BUFFER_DWORDS);
	if (!indices || !NT_SUCCESS(status)) {
		xsdk_log_write("draw cost probe: no memory for the buffers");
		return 1;
	}
	index_address = xsdk_gpu_address(indices);
	status = xsdk_gpu_open();
	if (!NT_SUCCESS(status)) {
		xsdk_log_write("draw cost probe: xsdk_gpu_open failed: 0x%08X", (unsigned)status);
		return 1;
	}
	bool clean = true;
	for (uint32_t round = 0; round < ROUNDS && clean; round++) {
		for (uint32_t variant = 0; variant < VARIANTS && clean; variant++)
			clean = time_variant(variant, round);
	}
	status = xsdk_gpu_close();
	if (!clean) {
		xsdk_log_write("draw cost probe: a recording overflowed or the stream didn't finish");
		return 1;
	}
	for (uint32_t variant = 0; variant < VARIANTS; variant++)
		report(variant);
	xsdk_gpu_command_buffer_free(&buffer);
	xsdk_gpu_free(indices);
	xsdk_log_write("draw cost probe: done; xsdk_gpu_close returned 0x%08X", (unsigned)status);
	return NT_SUCCESS(status) ? 0 : 1;
}
