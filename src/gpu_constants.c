#include "xsdk/gpu.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gpu_private.h"

// Constants in aligned whole groups, as the console takes them: a lone float constant hung the GPU (docs/HARDWARE.md).
enum {
	// Groups of 4 float constants, from a multiple of 4, ran on the console; libxenon writes 16 (xe.c:1320-1331).
	FLOAT_GROUP = 4,
	FLOAT_GROUP_DWORDS = FLOAT_GROUP * 4,
	// What the console loaded constants from; a group's 64 bytes.
	LOAD_ALIGNMENT_BYTES = 64,
	// And the 8 boolean words and 32 loop constants as one block from 0x4900 (xe.c:1365-1369).
	INTEGER_DWORDS = XSDK_BOOL_CONSTANT_WORDS + XSDK_LOOP_CONSTANTS,
	// A one-dword packet that does nothing, which libxenon pads with (xe.c:1217-1221).
	TYPE2_NOP = 0x80000000,
	// The padding never needs more than 3 dwords.
	ALIGN_DWORDS = 3,
};

// Each stage's float constants, and the boolean and loop words, as last set.
static uint32_t float_constants[2][XSDK_FLOAT_CONSTANTS * 4];
static uint32_t integer_constants[INTEGER_DWORDS];

// Xe_pAlign then one type-0 packet: the header sits 3 dwords into a 16-byte group, so the values start on one.
static void put_aligned(uint32_t first_register, const uint32_t *values, uint32_t count)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(ALIGN_DWORDS + 1 + count);
	uint32_t i = 0;
	while (((uint32_t)(uintptr_t)&packet[i] >> 2 & 3) != 3)
		packet[i++] = TYPE2_NOP;
	packet[i++] = (count - 1) << 16 | first_register;
	for (uint32_t v = 0; v < count; v++)
		packet[i++] = values[v];
	while (i < ALIGN_DWORDS + 1 + count)
		packet[i++] = TYPE2_NOP;
}

// Copies the floats in, then writes every whole group they touch.
static void set_floats(uint32_t stage, uint32_t first_register, uint32_t first, const float *values, uint32_t count)
{
	assert(count && first + count <= XSDK_FLOAT_CONSTANTS);
	memcpy(&float_constants[stage][first * 4], values, count * 4 * sizeof(float));
	const uint32_t first_group = first / FLOAT_GROUP;
	const uint32_t groups = (first + count - 1) / FLOAT_GROUP - first_group + 1;
	put_aligned(first_register + first_group * FLOAT_GROUP_DWORDS,
		&float_constants[stage][first_group * FLOAT_GROUP_DWORDS], groups * FLOAT_GROUP_DWORDS);
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
	volatile uint32_t *const packet = xsdk_gpu_reserve(4);
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
	memcpy(&integer_constants[first_word], words, count * sizeof(uint32_t));
	put_aligned(SHADER_CONSTANT_BOOL_000_031, integer_constants, INTEGER_DWORDS);
}

void xsdk_gpu_set_loop_constants(uint32_t first, const uint32_t *values, uint32_t count)
{
	assert(count && first + count <= XSDK_LOOP_CONSTANTS);
	memcpy(&integer_constants[XSDK_BOOL_CONSTANT_WORDS + first], values, count * sizeof(uint32_t));
	put_aligned(SHADER_CONSTANT_BOOL_000_031, integer_constants, INTEGER_DWORDS);
}
