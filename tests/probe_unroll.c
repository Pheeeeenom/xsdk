#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unroll_loops.h"
#include "xsdk/log.h"

// Times loops typical of game code built both ways, -O2 and -funroll-loops, side by side (SCOPE.md, milestone 35).

enum {
	ROUNDS = 9,
	VARIANTS = 2,
	// The time base ticks every 64 CPU clocks: 3192 MHz over 49.875 MHz (src/time.c).
	TIME_BASE_HZ = 49875000,
	CLOCKS_PER_TICK = 64,
	CHAINS = 8,
	CHAIN_ITERATIONS = 2000000,
	ARRAY_COUNT = 2048,
	ARRAY_PASSES = 4000,
	VERTEX_COUNT = 512,
	VERTEX_PASSES = 2000,
	COPY_WORDS = 2048,
	COPY_PASSES = 4000,
	HASH_BYTES = 8192,
	HASH_PASSES = 500,
	KEY_COUNT = 2048,
	KEY_PASSES = 1000,
};

typedef uint32_t (*Run)(const UnrollLoops *loops);

typedef struct {
	const char *name;
	const char *unit;
	// What one run does, in units and in float operations a unit.
	uint32_t units;
	uint32_t flops_per_unit;
	Run run;
} Loop;

static const UnrollLoops *const variants[VARIANTS] = { &unroll_loops_o2, &unroll_loops_unrolled };
static const char *const variant_names[VARIANTS] = { "-O2", "-funroll-loops" };

static float array_x[ARRAY_COUNT];
static float array_y[ARRAY_COUNT];
static float array_out[ARRAY_COUNT];
static Vec4 vertices_in[VERTEX_COUNT];
static Vec4 vertices_out[VERTEX_COUNT];
static uint32_t copy_in[COPY_WORDS];
static uint32_t copy_out[COPY_WORDS];
static uint8_t hash_input[HASH_BYTES];
static uint32_t keys_in[KEY_COUNT];
static uint32_t keys_out[KEY_COUNT];

static const Matrix4 matrix = { {
	{ 0.8f, -0.6f, 0.0f, 12.0f },
	{ 0.6f, 0.8f, 0.0f, -3.5f },
	{ 0.0f, 0.0f, 1.0f, 40.0f },
	{ 0.0f, 0.0f, 0.25f, 1.0f },
} };

static uint64_t time_base(void)
{
	uint32_t upper;
	uint32_t lower;
	uint32_t check;
	// The upper half can tick between the two reads in 32-bit mode (src/time.c).
	do {
		__asm__ volatile("mftbu %0" : "=r"(upper) : : "memory");
		__asm__ volatile("mftb %0" : "=r"(lower) : : "memory");
		__asm__ volatile("mftbu %0" : "=r"(check) : : "memory");
	} while (upper != check);
	return (uint64_t)upper << 32 | lower;
}

static uint32_t checksum(const void *data, uint32_t size_bytes)
{
	const uint32_t *const words = data;
	uint32_t sum = 0;
	for (uint32_t i = 0; i < size_bytes / 4; i++)
		sum = (sum << 5 | sum >> 27) ^ words[i];
	return sum;
}

static uint32_t run_chains(const UnrollLoops *loops)
{
	const double sum = loops->multiply_add_chains(CHAIN_ITERATIONS);
	uint64_t bits;
	memcpy(&bits, &sum, sizeof bits);
	return (uint32_t)(bits >> 32) ^ (uint32_t)bits;
}

static uint32_t run_arrays(const UnrollLoops *loops)
{
	for (uint32_t pass = 0; pass < ARRAY_PASSES; pass++)
		loops->multiply_add_arrays(array_out, array_x, array_y, 0.5f, ARRAY_COUNT);
	return checksum(array_out, sizeof array_out);
}

static uint32_t run_transform(const UnrollLoops *loops)
{
	for (uint32_t pass = 0; pass < VERTEX_PASSES; pass++)
		loops->transform(vertices_out, vertices_in, &matrix, VERTEX_COUNT);
	return checksum(vertices_out, sizeof vertices_out);
}

static uint32_t run_copy(const UnrollLoops *loops)
{
	for (uint32_t pass = 0; pass < COPY_PASSES; pass++)
		loops->copy_words(copy_out, copy_in, COPY_WORDS);
	return checksum(copy_out, sizeof copy_out);
}

static uint32_t run_hash_bytes(const UnrollLoops *loops)
{
	uint32_t sum = 0;
	for (uint32_t pass = 0; pass < HASH_PASSES; pass++)
		sum += loops->hash_bytes(hash_input, HASH_BYTES);
	return sum;
}

static uint32_t run_hash_keys(const UnrollLoops *loops)
{
	for (uint32_t pass = 0; pass < KEY_PASSES; pass++)
		loops->hash_keys(keys_out, keys_in, KEY_COUNT);
	return checksum(keys_out, sizeof keys_out);
}

// A transform is 16 multiplies and 12 adds; a multiply-add counts as two.
static const Loop loops[] = {
	{ "8 multiply-add chains", "multiply-add", CHAINS * CHAIN_ITERATIONS, 2, run_chains },
	{ "multiply-add over arrays", "result", ARRAY_COUNT * ARRAY_PASSES, 2, run_arrays },
	{ "4x4 matrix times vectors", "vertex", VERTEX_COUNT * VERTEX_PASSES, 28, run_transform },
	{ "word copy", "word", COPY_WORDS * COPY_PASSES, 0, run_copy },
	{ "FNV-1a over bytes", "byte", HASH_BYTES * HASH_PASSES, 0, run_hash_bytes },
	{ "MurmurHash3 finalizer over keys", "key", KEY_COUNT * KEY_PASSES, 0, run_hash_keys },
};

enum { LOOPS = sizeof loops / sizeof loops[0] };

static uint64_t ticks[LOOPS][VARIANTS][ROUNDS];
static uint32_t results[LOOPS][VARIANTS];

static void fill_inputs(void)
{
	for (uint32_t i = 0; i < ARRAY_COUNT; i++) {
		array_x[i] = (float)(i % 97) * 0.25f;
		array_y[i] = (float)(i % 89) * 0.5f;
	}
	for (uint32_t i = 0; i < VERTEX_COUNT; i++)
		vertices_in[i] = (Vec4){ (float)(i % 31), (float)(i % 37) * 0.5f, (float)(i % 41) * 0.25f, 1.0f };
	for (uint32_t i = 0; i < COPY_WORDS; i++)
		copy_in[i] = i * 2654435761u;
	for (uint32_t i = 0; i < HASH_BYTES; i++)
		hash_input[i] = (uint8_t)(i * 31 + 7);
	for (uint32_t i = 0; i < KEY_COUNT; i++)
		keys_in[i] = i * 2654435761u + 12345;
}

// Runs each loop both ways; false if the two ways' results differ.
static bool time_round(uint32_t round)
{
	bool same = true;
	for (uint32_t loop = 0; loop < LOOPS; loop++) {
		for (uint32_t i = 0; i < VARIANTS; i++) {
			// Each goes first in alternate rounds.
			const uint32_t variant = (round + i) % VARIANTS;
			const uint64_t start = time_base();
			results[loop][variant] = loops[loop].run(variants[variant]);
			ticks[loop][variant][round] = time_base() - start;
		}
		same = same && results[loop][0] == results[loop][1];
	}
	return same;
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

static void report(uint32_t loop)
{
	const Loop *const l = &loops[loop];
	double best_clocks[VARIANTS];
	for (uint32_t variant = 0; variant < VARIANTS; variant++) {
		uint64_t *const t = ticks[loop][variant];
		sort_ticks(t, ROUNDS);
		best_clocks[variant] = (double)t[0] * CLOCKS_PER_TICK / l->units;
		const double median_clocks = (double)t[ROUNDS / 2] * CLOCKS_PER_TICK / l->units;
		const double worst_clocks = (double)t[ROUNDS - 1] * CLOCKS_PER_TICK / l->units;
		xsdk_log_write("unroll probe: %s, %s: %.3f clocks a %s at best, %.3f the median, %.3f the worst", l->name,
			variant_names[variant], best_clocks[variant], l->unit, median_clocks, worst_clocks);
		if (l->flops_per_unit) {
			const double gflops = (double)l->units * l->flops_per_unit * TIME_BASE_HZ / (double)t[0] / 1e9;
			xsdk_log_write("unroll probe: %s, %s: %.2f GFLOPS at best", l->name, variant_names[variant], gflops);
		}
	}
	xsdk_log_write(
		"unroll probe: %s: -funroll-loops %.3f times as fast as -O2", l->name, best_clocks[0] / best_clocks[1]);
}

int main(void)
{
	xsdk_log_write("unroll probe: %u rounds of %u loops, each built as GCC's -O2 builds it and with -funroll-loops",
		(unsigned)ROUNDS, (unsigned)LOOPS);
	fill_inputs();
	bool same = true;
	for (uint32_t round = 0; round < ROUNDS; round++)
		same = time_round(round) && same;
	for (uint32_t loop = 0; loop < LOOPS; loop++)
		report(loop);
	xsdk_log_write("unroll probe: every loop's results %s both ways", same ? "were the same" : "differed");
	return same ? 0 : 1;
}
