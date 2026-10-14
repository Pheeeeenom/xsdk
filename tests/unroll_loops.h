#ifndef XSDK_UNROLL_LOOPS_H
#define XSDK_UNROLL_LOOPS_H

#include <stdint.h>

typedef struct {
	float x;
	float y;
	float z;
	float w;
} Vec4;

typedef struct {
	float rows[4][4];
} Matrix4;

// Loops typical of game code, built twice from unroll_loops.c: as GCC's -O2 builds them, and with -funroll-loops.
typedef struct {
	// Eight independent chains of double multiply-adds; returns their sum.
	double (*multiply_add_chains)(uint32_t iterations);
	void (*multiply_add_arrays)(float *out, const float *x, const float *y, float a, uint32_t count);
	void (*transform)(Vec4 *out, const Vec4 *in, const Matrix4 *matrix, uint32_t count);
	void (*copy_words)(uint32_t *out, const uint32_t *in, uint32_t count);
	uint32_t (*hash_bytes)(const uint8_t *bytes, uint32_t count);
	void (*hash_keys)(uint32_t *out, const uint32_t *keys, uint32_t count);
} UnrollLoops;

extern const UnrollLoops unroll_loops_o2;
extern const UnrollLoops unroll_loops_unrolled;

#endif
