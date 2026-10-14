// Local probe: does the same 64-bit integer work, done twice from the same seed, ever come out differently on the
// console? Once in registers, once through memory.
#include <stdint.h>
#include <stdio.h>

#include "xsdk/thread.h"

enum { TRIALS = 20000, ROUNDS = 10000, HARDWARE_THREADS = 6 };

__attribute__((noinline)) static uint64_t churn_registers(uint64_t a, uint64_t b, uint32_t rounds)
{
	uint64_t c = 0x9E3779B97F4A7C15ull, d = a ^ b, e = ~a, f = b + 1, g = a * 3, h = b ^ 0x5555;
	for (uint32_t i = 0; i < rounds; i++) {
		a ^= a << 13;
		a ^= a >> 7;
		a ^= a << 17;
		b = (b << 23 | b >> 41) ^ a;
		c += a ^ (b & ~d);
		d = (d << 31 | d >> 33) ^ c;
		e ^= f & ~g;
		f = (f << 7 | f >> 57) + e;
		g ^= h + c;
		h = (h << 3 | h >> 61) ^ g;
	}
	return a ^ b ^ c ^ d ^ e ^ f ^ g ^ h;
}

__attribute__((noinline)) static uint64_t churn_memory(uint64_t a0, uint64_t b0, uint32_t rounds)
{
	volatile uint64_t v[8] = { a0, b0, 0x9E3779B97F4A7C15ull, a0 ^ b0, ~a0, b0 + 1, a0 * 3, b0 ^ 0x5555 };
	for (uint32_t i = 0; i < rounds; i++) {
		v[0] ^= v[0] << 13;
		v[0] ^= v[0] >> 7;
		v[0] ^= v[0] << 17;
		v[1] = (v[1] << 23 | v[1] >> 41) ^ v[0];
		v[2] += v[0] ^ (v[1] & ~v[3]);
		v[3] = (v[3] << 31 | v[3] >> 33) ^ v[2];
		v[4] ^= v[5] & ~v[6];
		v[5] = (v[5] << 7 | v[5] >> 57) + v[4];
		v[6] ^= v[7] + v[2];
		v[7] = (v[7] << 3 | v[7] >> 61) ^ v[6];
	}
	return v[0] ^ v[1] ^ v[2] ^ v[3] ^ v[4] ^ v[5] ^ v[6] ^ v[7];
}

static unsigned compare(uint64_t (*churn)(uint64_t, uint64_t, uint32_t), uint32_t trials)
{
	unsigned mismatches = 0;
	for (uint32_t t = 0; t < trials; t++) {
		const uint64_t a = 0x0123456789ABCDEFull * (t + 1), b = 0xFEDCBA9876543210ull ^ t;
		if (churn(a, b, ROUNDS) != churn(a, b, ROUNDS))
			mismatches++;
	}
	return mismatches;
}

int main(void)
{
	for (uint32_t thread = 0; thread < HARDWARE_THREADS; thread += 2) {
		(void)xsdk_thread_move(thread);
		printf("alu: hardware thread %u: in registers %u of %d trials differed; through memory %u of %d\n",
			(unsigned)xsdk_thread_hardware_thread(), compare(churn_registers, TRIALS), TRIALS,
			compare(churn_memory, TRIALS / 4), TRIALS / 4);
	}
	return 0;
}
