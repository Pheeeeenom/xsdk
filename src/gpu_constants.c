#include "xsdk/gpu.h"

#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "gpu_private.h"

// One type-0 packet over consecutive registers, as libxenon uploads its constants (libxenon:drivers/xenos/xe.c:1330).
static void put_registers(uint32_t first_register, const uint32_t *values, uint32_t count)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(count + 1);
	packet[0] = (count - 1) << 16 | first_register;
	for (uint32_t i = 0; i < count; i++)
		packet[1 + i] = values[i];
}

static void put_floats(uint32_t first_register, const float *values, uint32_t count)
{
	volatile uint32_t *const packet = xsdk_gpu_reserve(count + 1);
	packet[0] = (count - 1) << 16 | first_register;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t bits;
		memcpy(&bits, &values[i], sizeof bits);
		packet[1 + i] = bits;
	}
}

void xsdk_gpu_set_vertex_constants(uint32_t first, const float *values, uint32_t count)
{
	assert(count && first + count <= XSDK_FLOAT_CONSTANTS);
	put_floats(SHADER_CONSTANT_000_X + first * 4, values, count * 4);
}

// The pixel shader's constant 0 is the 256th; xsdk_gpu_open sets the split (src/gpu_setup.c).
void xsdk_gpu_set_pixel_constants(uint32_t first, const float *values, uint32_t count)
{
	assert(count && first + count <= XSDK_FLOAT_CONSTANTS);
	put_floats(SHADER_CONSTANT_256_X + first * 4, values, count * 4);
}

void xsdk_gpu_set_bool_constants(uint32_t first_word, const uint32_t *words, uint32_t count)
{
	assert(count && first_word + count <= XSDK_BOOL_CONSTANT_WORDS);
	put_registers(SHADER_CONSTANT_BOOL_000_031 + first_word, words, count);
}

void xsdk_gpu_set_loop_constants(uint32_t first, const uint32_t *values, uint32_t count)
{
	assert(count && first + count <= XSDK_LOOP_CONSTANTS);
	put_registers(SHADER_CONSTANT_LOOP_00 + first, values, count);
}
