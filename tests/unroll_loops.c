#include "unroll_loops.h"

#include <stdint.h>

// The Makefile builds this file twice, naming the table each time with UNROLL_LOOPS.

static double multiply_add_chains(uint32_t iterations)
{
	double a0 = 0.0, a1 = 0.1, a2 = 0.2, a3 = 0.3, a4 = 0.4, a5 = 0.5, a6 = 0.6, a7 = 0.7;
	// Each chain closes in on 1, so no value grows or falls into denormals.
	const double scale = 0.999999;
	const double step = 0.000001;
	for (uint32_t i = 0; i < iterations; i++) {
		a0 = a0 * scale + step;
		a1 = a1 * scale + step;
		a2 = a2 * scale + step;
		a3 = a3 * scale + step;
		a4 = a4 * scale + step;
		a5 = a5 * scale + step;
		a6 = a6 * scale + step;
		a7 = a7 * scale + step;
	}
	return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

static void multiply_add_arrays(float *out, const float *x, const float *y, float a, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		out[i] = a * x[i] + y[i];
}

static void transform(Vec4 *out, const Vec4 *in, const Matrix4 *matrix, uint32_t count)
{
	const Matrix4 m = *matrix;
	for (uint32_t i = 0; i < count; i++) {
		const Vec4 v = in[i];
		out[i] = (Vec4){
			m.rows[0][0] * v.x + m.rows[0][1] * v.y + m.rows[0][2] * v.z + m.rows[0][3] * v.w,
			m.rows[1][0] * v.x + m.rows[1][1] * v.y + m.rows[1][2] * v.z + m.rows[1][3] * v.w,
			m.rows[2][0] * v.x + m.rows[2][1] * v.y + m.rows[2][2] * v.z + m.rows[2][3] * v.w,
			m.rows[3][0] * v.x + m.rows[3][1] * v.y + m.rows[3][2] * v.z + m.rows[3][3] * v.w,
		};
	}
}

static void copy_words(uint32_t *out, const uint32_t *in, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++)
		out[i] = in[i];
}

// 32-bit FNV-1a (http://www.isthe.com/chongo/tech/comp/fnv/).
static uint32_t hash_bytes(const uint8_t *bytes, uint32_t count)
{
	uint32_t hash = 2166136261u;
	for (uint32_t i = 0; i < count; i++)
		hash = (hash ^ bytes[i]) * 16777619u;
	return hash;
}

// MurmurHash3's finalizer (https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp).
static void hash_keys(uint32_t *out, const uint32_t *keys, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		uint32_t h = keys[i];
		h ^= h >> 16;
		h *= 0x85EBCA6Bu;
		h ^= h >> 13;
		h *= 0xC2B2AE35u;
		h ^= h >> 16;
		out[i] = h;
	}
}

const UnrollLoops UNROLL_LOOPS = {
	.multiply_add_chains = multiply_add_chains,
	.multiply_add_arrays = multiply_add_arrays,
	.transform = transform,
	.copy_words = copy_words,
	.hash_bytes = hash_bytes,
	.hash_keys = hash_keys,
};
