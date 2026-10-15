#include "xsdk/gpu.h"

#include <altivec.h>
#include <assert.h>
#include <stdint.h>

#include "gpu_private.h"

// Constants in aligned whole groups, as the console takes them: a lone float constant hung the GPU (docs/HARDWARE.md).
enum {
	// Groups of 4 float constants, from a multiple of 4, ran on the console; libxenon writes 16 (xe.c:1320-1331).
	FLOAT_GROUP = 4,
	// What the console loaded constants from; a group's 64 bytes.
	LOAD_ALIGNMENT_BYTES = 64,
	// And the 8 boolean words and 32 loop constants as one block from 0x4900 (xe.c:1365-1369).
	INTEGER_DWORDS = XSDK_BOOL_CONSTANT_WORDS + XSDK_LOOP_CONSTANTS,
	// A one-dword packet that does nothing, which libxenon pads with (xe.c:1217-1221).
	TYPE2_NOP = 0x80000000,
	// The padding never needs more than 3 dwords.
	ALIGN_DWORDS = 3,
	// Where in its 16-byte group the header goes, so the values start on the next.
	HEADER_SLOT = 3,
};

typedef __vector float Quad;

// Each stage's float constants, aligned for 16-byte loads and stores, and the boolean and loop words, as last set.
static float float_constants[2][XSDK_FLOAT_CONSTANTS * 4] __attribute__((aligned(16)));
static uint32_t integer_constants[INTEGER_DWORDS];

// Xe_pAlign then the header of one type-0 packet of count dwords: NOPs until the header sits 3 dwords into a 16-byte
// group, so the values start on one. Returns how many NOPs.
static inline uint32_t put_header(volatile uint32_t *packet, uint32_t first_register, uint32_t count)
{
	const uint32_t before = (HEADER_SLOT - ((uint32_t)(uintptr_t)packet >> 2)) & 3;
	for (uint32_t i = 0; i < before; i++)
		packet[i] = TYPE2_NOP;
	packet[before] = (count - 1) << 16 | first_register;
	return before;
}

// The NOPs after the values, so the packet fills what was reserved.
static inline void put_padding(volatile uint32_t *after_values, uint32_t before)
{
	for (uint32_t i = before; i < ALIGN_DWORDS; i++)
		*after_values++ = TYPE2_NOP;
}

// One aligned type-0 packet of a run of kept dwords, the count from start new; each new one goes into the run as it
// goes into the packet, as reading the run back just after storing into it would stall the console's core on each
// (docs/HARDWARE.md, "The cost of a draw").
static void put_aligned(
	uint32_t first_register, uint32_t *run, uint32_t run_dwords, uint32_t start, const uint32_t *values, uint32_t count)
{
	assert(start + count <= run_dwords);
	volatile uint32_t *const packet = gpu_reserve(ALIGN_DWORDS + 1 + run_dwords);
	const uint32_t before = put_header(packet, first_register, run_dwords);
	volatile uint32_t *out = &packet[before + 1];
	for (uint32_t i = 0; i < start; i++)
		*out++ = run[i];
	for (uint32_t i = 0; i < count; i++) {
		run[start + i] = values[i];
		*out++ = values[i];
	}
	for (uint32_t i = start + count; i < run_dwords; i++)
		*out++ = run[i];
	put_padding(out, before);
}

// A constant from anywhere 4-byte aligned: the two quadwords it spans, put together.
static inline Quad load_constant(const float *value)
{
	return vec_perm(vec_ld(0, value), vec_ld(15, value), vec_lvsl(0, value));
}

// Every whole group the floats touch, the stage's kept constants around them, a constant to a 16-byte store: the
// packet's alignment puts each on 16 bytes, and VMX stores land in write-combined memory (docs/HARDWARE.md, "Copies
// into write-combined and cached memory"). Dword stores, keeping the new ones, cost the console half again as much
// (docs/HARDWARE.md, "The cost of a draw").
static void set_floats(uint32_t stage, uint32_t first_register, uint32_t first, const float *values, uint32_t count)
{
	assert(count && first + count <= XSDK_FLOAT_CONSTANTS);
	const uint32_t group_first = first / FLOAT_GROUP * FLOAT_GROUP;
	const uint32_t group_end = (first + count + FLOAT_GROUP - 1) / FLOAT_GROUP * FLOAT_GROUP;
	const uint32_t dwords = (group_end - group_first) * 4;
	volatile uint32_t *const packet = gpu_reserve(ALIGN_DWORDS + 1 + dwords);
	const uint32_t before = put_header(packet, first_register + group_first * 4, dwords);
	float *const out = (float *)&packet[before + 1];
	assert(((uintptr_t)out & 15) == 0);
	float *const kept = float_constants[stage];
	for (uint32_t c = group_first; c < group_end; c++) {
		Quad constant;
		if (c - first < count) {
			constant = load_constant(&values[(c - first) * 4]);
			vec_st(constant, 0, &kept[c * 4]);
		} else {
			constant = vec_ld(0, &kept[c * 4]);
		}
		vec_st(constant, 0, &out[(c - group_first) * 4]);
	}
	put_padding(&packet[before + 1 + dwords], before);
}

void xsdk_gpu_set_vertex_constants(uint32_t first, const float *values, uint32_t count)
{
	set_floats(0, SHADER_CONSTANT_000_X, first, values, count);
}

// The pixel shader's constant 0 is the 256th; xsdk_gpu_open sets the split (src/gpu_setup.c).
void xsdk_gpu_set_pixel_constants(uint32_t first, const float *values, uint32_t count)
{
	set_floats(1, SHADER_CONSTANT_256_X, first, values, count);
}

// LOAD_ALU_CONSTANT: the address, then type 0 for float constants and the first register's index, then the dwords
// (xenia:src/xenia/gpu/command_processor.cc:1477-1515).
static void load_floats(uint32_t first_register, uint32_t first, uint32_t address, uint32_t count)
{
	assert(count && first % FLOAT_GROUP == 0 && count % FLOAT_GROUP == 0 && first + count <= XSDK_FLOAT_CONSTANTS);
	assert(address % LOAD_ALIGNMENT_BYTES == 0);
	volatile uint32_t *const packet = gpu_reserve(4);
	packet[0] = xsdk_gpu_type3(PM4_LOAD_ALU_CONSTANT, 3);
	packet[1] = address;
	packet[2] = first_register - SHADER_CONSTANT_000_X + first * 4;
	packet[3] = count * 4;
}

void xsdk_gpu_load_vertex_constants(uint32_t first, uint32_t address, uint32_t count)
{
	load_floats(SHADER_CONSTANT_000_X, first, address, count);
}

void xsdk_gpu_load_pixel_constants(uint32_t first, uint32_t address, uint32_t count)
{
	load_floats(SHADER_CONSTANT_256_X, first, address, count);
}

void xsdk_gpu_set_bool_constants(uint32_t first_word, const uint32_t *words, uint32_t count)
{
	assert(count && first_word + count <= XSDK_BOOL_CONSTANT_WORDS);
	put_aligned(SHADER_CONSTANT_BOOL_000_031, integer_constants, INTEGER_DWORDS, first_word, words, count);
}

void xsdk_gpu_set_loop_constants(uint32_t first, const uint32_t *values, uint32_t count)
{
	assert(count && first + count <= XSDK_LOOP_CONSTANTS);
	put_aligned(SHADER_CONSTANT_BOOL_000_031, integer_constants, INTEGER_DWORDS, XSDK_BOOL_CONSTANT_WORDS + first,
		values, count);
}
